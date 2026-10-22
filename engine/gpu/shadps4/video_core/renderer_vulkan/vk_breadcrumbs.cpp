// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <string>
#include <map>
#include <chrono>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include "bbport_toggles.h"
#include "video_core/renderer_vulkan/vk_breadcrumbs.h"
#include <vector>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_optiscaler.h"

extern "C" int runtime_memory_vma_info(uintptr_t address, int* prot, int* type, uintptr_t* end);

#include <vk_mem_alloc.h>

namespace Vulkan::Breadcrumbs {

namespace {

constexpr u32 MaxStreams = 16;
constexpr u32 StreamStride = 64; // bytes of the marker buffer per stream (a cache line)
constexpr u64 RingSize = 1u << 15;
constexpr u32 ArgSlots = 4096;                     // indirect dispatch arguments, 16 bytes each
constexpr u64 ArgsOffset = MaxStreams * StreamStride; // where they start in the buffer

struct Entry {
    u64 id;
    Crumb crumb;
};

struct Stream {
    const char* name = nullptr;
    std::atomic<u64> next{1};
    std::unique_ptr<Entry[]> ring;
};

std::array<Stream, MaxStreams> streams;
std::atomic<u32> num_streams{0};
bool enabled = false;
std::once_flag init_once;
vk::Buffer marker_buffer{};
VmaAllocation marker_allocation{};
volatile u32* markers = nullptr;
volatile u32* args = nullptr;
std::atomic<u32> next_args_slot{0};
/// What each argument slot was last used for.
struct ArgsNote {
    u64 id;
    u32 stream;
    u64 hash;
};
std::array<ArgsNote, ArgSlots> args_notes{};

struct AddressBinding {
    u64 base;
    u64 size;
    u64 handle;
    VkObjectType type;
    std::chrono::steady_clock::time_point freed;
    char name[112];
};
struct FreedRing {
    std::unique_ptr<AddressBinding[]> entries;
    u64 capacity = 0;
    u64 next = 0;
    void Push(const AddressBinding& entry) {
        if (!entries) {
            entries = std::make_unique<AddressBinding[]>(capacity);
        }
        entries[next++ % capacity] = entry;
    }
};
std::mutex address_mutex;
std::map<u64, AddressBinding> address_bound;
FreedRing freed_images{.capacity = 32768};
FreedRing freed_others{.capacity = 65536};

const char* ObjectTypeName(VkObjectType type) {
    switch (type) {
    case VK_OBJECT_TYPE_IMAGE:
        return "image";
    case VK_OBJECT_TYPE_BUFFER:
        return "buffer";
    case VK_OBJECT_TYPE_DEVICE_MEMORY:
        return "memory";
    case VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR:
        return "acceleration structure";
    default:
        return "object";
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL AddressReportCallback(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                     VkDebugUtilsMessageTypeFlagsEXT type,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                     void*) {
    if (!(type & VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT) || !data) {
        return VK_FALSE;
    }
    const VkDeviceAddressBindingCallbackDataEXT* binding = nullptr;
    for (auto* next = static_cast<const VkBaseInStructure*>(data->pNext); next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT) {
            binding = reinterpret_cast<const VkDeviceAddressBindingCallbackDataEXT*>(next);
            break;
        }
    }
    if (!binding || binding->size == 0) {
        return VK_FALSE;
    }
    AddressBinding entry{.base = binding->baseAddress, .size = binding->size, .name = {}};
    if (data->objectCount > 0 && data->pObjects) {
        entry.handle = data->pObjects[0].objectHandle;
        entry.type = data->pObjects[0].objectType;
        if (data->pObjects[0].pObjectName) {
            std::snprintf(entry.name, sizeof(entry.name), "%s", data->pObjects[0].pObjectName);
        }
    }
    std::scoped_lock lock{address_mutex};
    if (binding->bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT) {
        address_bound[entry.base] = entry;
        return VK_FALSE;
    }
    if (auto it = address_bound.find(entry.base); it != address_bound.end()) {
        if (!entry.name[0]) {
            std::memcpy(entry.name, it->second.name, sizeof(entry.name));
        }
        if (entry.type == VK_OBJECT_TYPE_UNKNOWN) {
            entry.type = it->second.type;
        }
        address_bound.erase(it);
    }
    entry.freed = std::chrono::steady_clock::now();
    (entry.type == VK_OBJECT_TYPE_IMAGE ? freed_images : freed_others).Push(entry);
    return VK_FALSE;
}

bool FindFreed(const FreedRing& ring, u64 address, std::chrono::steady_clock::time_point now) {
    const u64 count = std::min(ring.next, ring.capacity);
    for (u64 i = 0; i < count; ++i) {
        const auto& freed = ring.entries[(ring.next - 1 - i) % ring.capacity];
        if (address >= freed.base && address < freed.base + freed.size) {
            std::printf("    in a FREED %s %#llx [%#llx, +%#llx), freed %lld ms before the crash: %s\n",
                        ObjectTypeName(freed.type), static_cast<unsigned long long>(freed.handle),
                        static_cast<unsigned long long>(freed.base),
                        static_cast<unsigned long long>(freed.size),
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                   now - freed.freed)
                                                   .count()),
                        freed.name);
            return true;
        }
    }
    return false;
}

void DescribeRing(const char* what, const FreedRing& ring, std::chrono::steady_clock::time_point now) {
    const u64 count = std::min(ring.next, ring.capacity);
    if (count == 0) {
        std::printf("    %s: none freed\n", what);
        return;
    }
    const auto& oldest = ring.entries[(ring.next - count) % ring.capacity];
    std::printf("    %s: %llu freed in all, the last %llu kept (back to %lld ms before the crash)\n", what,
                static_cast<unsigned long long>(ring.next), static_cast<unsigned long long>(count),
                static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - oldest.freed).count()));
}

void DescribeFaultAddress(u64 address) {
    std::scoped_lock lock{address_mutex};
    auto it = address_bound.upper_bound(address);
    if (it != address_bound.begin()) {
        const auto& bound = std::prev(it)->second;
        if (address < bound.base + bound.size) {
            std::printf("    in a live %s %#llx [%#llx, +%#llx) %s\n", ObjectTypeName(bound.type),
                        static_cast<unsigned long long>(bound.handle),
                        static_cast<unsigned long long>(bound.base),
                        static_cast<unsigned long long>(bound.size), bound.name);
            return;
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (freed_images.entries && FindFreed(freed_images, address, now)) {
        return;
    }
    if (freed_others.entries && FindFreed(freed_others, address, now)) {
        return;
    }
    std::printf("    not in any memory bound now (%zu bound) or freed recently\n", address_bound.size());
    DescribeRing("images", freed_images, now);
    DescribeRing("other objects", freed_others, now);
    if (it != address_bound.begin()) {
        const auto& below = std::prev(it)->second;
        std::printf("    nearest below: %s [%#llx, +%#llx) %s\n", ObjectTypeName(below.type),
                    static_cast<unsigned long long>(below.base),
                    static_cast<unsigned long long>(below.size), below.name);
    }
    if (it != address_bound.end()) {
        std::printf("    nearest above: %s [%#llx, +%#llx) %s\n", ObjectTypeName(it->second.type),
                    static_cast<unsigned long long>(it->second.base),
                    static_cast<unsigned long long>(it->second.size), it->second.name);
    }
}

vk::Device fault_device{};

void Init(const Instance& instance) {
    if (instance.IsDeviceFaultSupported()) {
        fault_device = instance.GetDevice();
    }
    const char* env = std::getenv("BB_BREADCRUMBS");
    if (env && env[0] == '0') {
        return;
    }
    if (!instance.IsBufferMarkerSupported()) {
        std::printf("GPU breadcrumbs: VK_AMD_buffer_marker unavailable, GPU hangs will not be "
                    "located\n");
        return;
    }
    const VkBufferCreateInfo buffer_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = ArgsOffset + ArgSlots * 16,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
    };
    VkBuffer buffer{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer,
                        &marker_allocation, &info) != VK_SUCCESS ||
        !info.pMappedData) {
        std::printf("GPU breadcrumbs: no host-visible buffer, GPU hangs will not be located\n");
        return;
    }
    marker_buffer = vk::Buffer{buffer};
    markers = static_cast<volatile u32*>(info.pMappedData);
    for (u32 i = 0; i < (ArgsOffset + ArgSlots * 16) / sizeof(u32); ++i) {
        markers[i] = 0;
    }
    args = markers + ArgsOffset / sizeof(u32);
    enabled = true;
}

/// Guest memory that may be gone by now: read only where the runtime has it mapped (its direct
/// memory is a GPU driver mapping, which process_vm_readv cannot read).
size_t ReadGuest(u64 address, void* out, size_t size) {
    int prot = 0, type = 0;
    uintptr_t end = 0;
    if (!address || !runtime_memory_vma_info(address, &prot, &type, &end) || end <= address) {
        return 0;
    }
    size = std::min<size_t>(size, end - address);
    std::memcpy(out, reinterpret_cast<const void*>(address), size);
    return size;
}

const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::Pass:
        return "pass";
    case Kind::Draw:
        return "draw";
    case Kind::DrawIndexed:
        return "indexed draw";
    case Kind::DrawIndirect:
        return "indirect draw";
    case Kind::Dispatch:
        return "dispatch";
    case Kind::DispatchIndirect:
        return "indirect dispatch";
    }
    return "?";
}

void PrintEntry(const Stream& stream, u64 id, const char* label) {
    const Entry& entry = stream.ring[id & (RingSize - 1)];
    if (entry.id != id) {
        std::printf("    #%llu %s: no longer kept\n", (unsigned long long)id, label);
        return;
    }
    const Crumb& c = entry.crumb;
    std::printf("    #%llu %s: %s", (unsigned long long)id, label, KindName(c.kind));
    switch (c.kind) {
    case Kind::Pass:
        std::printf(" %s", c.name ? c.name : "?");
        break;
    case Kind::Draw:
    case Kind::DrawIndexed:
        std::printf(" vs %016llx ps %016llx, %u %s x %u instances", (unsigned long long)c.hash[0],
                    (unsigned long long)c.hash[1], c.count[0],
                    c.kind == Kind::Draw ? "vertices" : "indices", c.count[1]);
        break;
    case Kind::DrawIndirect: {
        std::printf(" vs %016llx ps %016llx, up to %u draws, stride %u, arguments at %#llx",
                    (unsigned long long)c.hash[0], (unsigned long long)c.hash[1], c.count[0],
                    c.count[1], (unsigned long long)c.address);
        u32 now[5]{};
        if (ReadGuest(c.address, now, sizeof(now)) == sizeof(now)) {
            std::printf(" (guest memory now %u %u %u %u %u)", now[0], now[1], now[2], now[3], now[4]);
        }
        break;
    }
    case Kind::Dispatch:
        std::printf(" cs %016llx, %ux%ux%u groups", (unsigned long long)c.hash[0], c.count[0],
                    c.count[1], c.count[2]);
        break;
    case Kind::DispatchIndirect: {
        std::printf(" cs %016llx, arguments at %#llx", (unsigned long long)c.hash[0],
                    (unsigned long long)c.address);
        if (c.args_slot < ArgSlots) {
            const volatile u32* read = args + c.args_slot * 4;
            std::printf(" (the GPU read %ux%ux%u groups)", read[0], read[1], read[2]);
        }
        std::printf(c.count[0] ? ", in place" : ", a copy");
        u32 now[3]{};
        if (ReadGuest(c.address, now, sizeof(now)) == sizeof(now)) {
            std::printf(" (guest memory now %ux%ux%u)", now[0], now[1], now[2]);
        }
        break;
    }
    }
    for (u32 i = 0; i < 2; ++i) {
        if (c.program[i]) {
            std::printf(", code %#llx", (unsigned long long)c.program[i]);
        }
    }
    std::printf("\n");
}

/// The guest code of the shaders of the stuck command, for disassembly (up to 64 KiB each:
/// the program and what follows, its OrbShdr header among it).
void DumpPrograms(const Stream& stream, u64 id) {
    const Entry& entry = stream.ring[id & (RingSize - 1)];
    if (entry.id != id) {
        return;
    }
    const char* dir = access("out", W_OK) == 0 ? "out/" : "";
    for (u32 i = 0; i < 2; ++i) {
        const u64 program = entry.crumb.program[i];
        if (!program) {
            continue;
        }
        std::array<u8, 64 * 1024> code{};
        size_t size = 0;
        // Page by page: the read stops at the first page that is not mapped.
        while (size < code.size()) {
            const size_t chunk = std::min<size_t>(4096 - ((program + size) & 4095), code.size() - size);
            const size_t read = ReadGuest(program + size, code.data() + size, chunk);
            size += read;
            if (read != chunk) {
                break;
            }
        }
        char path[256];
        std::snprintf(path, sizeof(path), "%shang_shader_%016llx.bin", dir,
                      (unsigned long long)entry.crumb.hash[i]);
        if (FILE* file = std::fopen(path, "wb")) {
            std::fwrite(code.data(), 1, size, file);
            std::fclose(file);
            std::printf("    shader %016llx code (%zu bytes from %#llx) saved to %s\n",
                        (unsigned long long)entry.crumb.hash[i], size,
                        (unsigned long long)program, path);
        }
    }
}

} // Anonymous namespace

u32 NewStream(const Instance& instance, const char* name) {
    std::call_once(init_once, [&] { Init(instance); });
    const u32 index = num_streams.fetch_add(1, std::memory_order_relaxed);
    if (index >= MaxStreams) {
        return MaxStreams;
    }
    streams[index].name = name;
    streams[index].ring = std::make_unique<Entry[]>(RingSize);
    return index;
}

bool Enabled() noexcept {
    return enabled;
}

u32 Note(u32 stream_index, const Crumb& crumb) {
    if (stream_index >= MaxStreams) {
        return 0;
    }
    Stream& stream = streams[stream_index];
    const u64 id = stream.next.fetch_add(1, std::memory_order_relaxed);
    Entry& entry = stream.ring[id & (RingSize - 1)];
    entry.crumb = crumb;
    entry.id = id;
    if (crumb.args_slot < ArgSlots) {
        args_notes[crumb.args_slot] = {id, stream_index, crumb.hash[0]};
    }
    return u32(id);
}

void Mark(vk::CommandBuffer cmdbuf, u32 stream, u32 id, bool after) {
    if (!enabled || stream >= MaxStreams) {
        return;
    }
    cmdbuf.writeBufferMarkerAMD(vk::PipelineStageFlagBits::eBottomOfPipe, marker_buffer,
                                stream * StreamStride, id * 2 + (after ? 1 : 0));
}

u32 ArgsSlot() {
    return enabled ? next_args_slot.fetch_add(1, std::memory_order_relaxed) % ArgSlots : ~0u;
}

void CopyArgs(vk::CommandBuffer cmdbuf, vk::Buffer source, u64 offset, u32 slot) {
    if (!enabled || slot >= ArgSlots) {
        return;
    }
    cmdbuf.copyBuffer(source, marker_buffer,
                      vk::BufferCopy{offset, ArgsOffset + u64(slot) * 16, 3 * sizeof(u32)});
}

namespace {
void Report(const char* title, const char* where);
}

void ReportDeviceLost(const char* where) {
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    static std::atomic<bool> done{false};
    if (reported.test_and_set()) {
        // Another thread is printing it, and both callers end the process right after: wait for
        // the report (a device lost on the submit thread and the GPU signal thread at once cut it
        // off mid-line), but not for good.
        for (int i = 0; i < 500 && !done.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return;
    }
    if (enabled) {
        Report("at device lost", where);
    }
    // The game's GPU range allocators (BB_FRAME_STATS, bbport_guest_hooks): Ebrietas' rock effect
    // took one from ~165 MB free to under 1 MB right before its device losses.
    for (std::size_t i = 0; i < BbStats::range_allocators.size(); ++i) {
        if (const u64 allocator = BbStats::range_allocators[i].load()) {
            std::printf("GPU range allocator %#llx at device lost: %llu live, %.1f MB free\n",
                        static_cast<unsigned long long>(allocator),
                        static_cast<unsigned long long>(BbStats::range_live[i].load()),
                        BbStats::range_bytes[i].load() / 1e6);
        }
    }
    ReportDeviceFault();
    if (const auto& opti = OptiScaler::Detect(); opti.loaded) {
        std::printf("Device lost with OptiScaler loaded (as %s, compatibility mode %s): it hooks "
                    "submissions and runs DLSS itself; to rule it out, remove %s and try again\n",
                    opti.module.c_str(), OptiScaler::Compat() ? "on" : "off", opti.module.c_str());
        if (!opti.frame_generation.empty()) {
            std::printf("OptiScaler's frame generation was on (FGOutput=%s): it runs through a D3D12 "
                        "bridge on Vulkan; set it to none in OptiScaler's menu first\n",
                        opti.frame_generation.c_str());
        }
        std::fflush(stdout);
    }
    std::fflush(stdout);
    done.store(true, std::memory_order_release);
}

vk::UniqueDebugUtilsMessengerEXT CreateAddressReport(vk::Instance instance) {
    const vk::DebugUtilsMessengerCreateInfoEXT info = {
        .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo |
                           vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose |
                           vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                           vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
        .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eDeviceAddressBinding,
        .pfnUserCallback = reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(AddressReportCallback),
    };
    auto [result, messenger] = instance.createDebugUtilsMessengerEXTUnique(info);
    if (result != vk::Result::eSuccess) {
        return {};
    }
    std::printf("GPU fault reports: memory binds are tracked (VK_EXT_device_address_binding_report)\n");
    return std::move(messenger);
}

void ReportDeviceFault() {
    const auto get = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceFaultInfoEXT;
    if (!fault_device || !get) {
        return;
    }
    const VkDevice device = static_cast<VkDevice>(fault_device);
    VkDeviceFaultCountsEXT counts{.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (get(device, &counts, nullptr) != VK_SUCCESS) {
        std::printf("GPU fault: the driver gave no fault report\n");
        return;
    }
    std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT> vendor(counts.vendorInfoCount);
    counts.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info{.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT,
                              .pAddressInfos = addresses.data(),
                              .pVendorInfos = vendor.data()};
    const VkResult result = get(device, &counts, &info);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        std::printf("GPU fault: the driver gave no fault report\n");
        return;
    }
    static constexpr const char* kinds[] = {"none",          "read",         "write",
                                            "execute",       "instruction",  "invalid instruction",
                                            "fault instruction"};
    std::printf("GPU fault: %s; %u addresses, %u vendor records\n",
                info.description[0] ? info.description : "(no description)",
                counts.addressInfoCount, counts.vendorInfoCount);
    for (u32 i = 0; i < counts.addressInfoCount; ++i) {
        const auto& a = addresses[i];
        const u32 kind = static_cast<u32>(a.addressType);
        std::printf("  %s at GPU address %#llx (+-%#llx)\n", kind < 7 ? kinds[kind] : "?",
                    static_cast<unsigned long long>(a.reportedAddress),
                    static_cast<unsigned long long>(a.addressPrecision));
        if (a.addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT ||
            a.addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT) {
            DescribeFaultAddress(a.reportedAddress);
        }
    }
    for (u32 i = 0; i < counts.vendorInfoCount; ++i) {
        const auto& v = vendor[i];
        std::printf("  vendor: %s (code %#llx, data %#llx)\n", v.description,
                    static_cast<unsigned long long>(v.vendorFaultCode),
                    static_cast<unsigned long long>(v.vendorFaultData));
    }
    std::fflush(stdout);
}

void ReportStuck(const char* where) {
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    if (enabled && !reported.test_and_set()) {
        Report("while the GPU makes no progress", where);
    }
}

namespace {
void Report(const char* title, const char* where) {
    std::printf("GPU breadcrumbs %s (%s): what each command stream finished\n", title, where);
    const u32 count = std::min(num_streams.load(), MaxStreams);
    for (u32 s = 0; s < count; ++s) {
        const Stream& stream = streams[s];
        const u64 last = stream.next.load() - 1;
        const u32 marker = markers[s * StreamStride / sizeof(u32)];
        std::printf("  stream %u (%s): %llu commands noted", s, stream.name,
                    (unsigned long long)last);
        if (last == 0) {
            std::printf("\n");
            continue;
        }
        if (marker == 0) {
            std::printf(", the GPU finished none\n");
            continue;
        }
        // The marker holds the low 32 bits of 2 * id + after.
        const u64 newest = last * 2 + 1;
        const u64 value = newest - u32(u32(newest) - marker);
        const u64 id = value / 2;
        if (value & 1) {
            if (id == last) {
                std::printf(", the GPU finished all\n");
                continue;
            }
            std::printf(", the GPU finished #%llu but did not start #%llu: stuck in the work "
                        "recorded between them (copies, clears, barriers) or before it\n",
                        (unsigned long long)id, (unsigned long long)(id + 1));
            PrintEntry(stream, id, "finished");
            PrintEntry(stream, id + 1, "next");
        } else {
            std::printf(", the GPU started #%llu and did not finish it (%llu noted after it)\n",
                        (unsigned long long)id, (unsigned long long)(last - id));
            if (id > 1) {
                PrintEntry(stream, id - 1, "finished");
            }
            PrintEntry(stream, id, "STUCK");
            std::fflush(stdout);
            DumpPrograms(stream, id);
        }
        // After a STUCK entry the one right behind it is the first later one (the GPU may run it
        // beside the stuck one; Ebrietas' reports never showed it); a finished one printed it as
        // "next".
        for (u64 next = id + ((value & 1) ? 2 : 1); next <= std::min(last, id + 4); ++next) {
            PrintEntry(stream, next, "later");
        }
    }
    std::fflush(stdout);
}
} // Anonymous namespace


void PrintProgress() {
    if (!enabled) {
        return;
    }
    std::printf("GPU breadcrumbs:");
    const u32 count = std::min(num_streams.load(), MaxStreams);
    std::array<u64, MaxStreams> finished{};
    for (u32 s = 0; s < count; ++s) {
        const u64 last = streams[s].next.load(std::memory_order_relaxed) - 1;
        if (last == 0) {
            continue;
        }
        const u32 marker = markers[s * StreamStride / sizeof(u32)];
        const u64 newest = last * 2 + 1;
        const u64 value = marker ? newest - u32(u32(newest) - marker) : 0;
        // 2n + 1: n finished; 2n: n started, n - 1 finished.
        finished[s] = value ? value / 2 - (value & 1 ? 0 : 1) : 0;
        std::printf(" stream %u (%s) %llu noted, %llu unfinished;", s, streams[s].name,
                    (unsigned long long)last, (unsigned long long)(last - finished[s]));
    }
    // The largest indirect dispatches among the last ArgSlots the GPU finished, one per shader:
    // counts that only grow from frame to frame end in a dispatch that never finishes.
    struct Largest {
        u64 hash;
        u64 groups;
        u32 dims[3];
    };
    std::array<Largest, 3> largest{};
    for (u32 slot = 0; slot < ArgSlots; ++slot) {
        const ArgsNote note = args_notes[slot];
        if (note.id == 0 || note.stream >= count || note.id > finished[note.stream]) {
            continue;
        }
        const u32 x = args[slot * 4], y = args[slot * 4 + 1], z = args[slot * 4 + 2];
        const u64 groups = u64(x) * y * z;
        auto it = std::find_if(largest.begin(), largest.end(),
                               [&](const Largest& l) { return l.hash == note.hash; });
        if (it == largest.end()) {
            it = std::min_element(largest.begin(), largest.end(),
                                  [](const Largest& a, const Largest& b) { return a.groups < b.groups; });
            if (it->groups >= groups) {
                continue;
            }
            *it = {note.hash, 0, {}};
        }
        if (groups >= it->groups) {
            *it = {note.hash, groups, {x, y, z}};
        }
    }
    std::sort(largest.begin(), largest.end(),
              [](const Largest& a, const Largest& b) { return a.groups > b.groups; });
    std::printf(" largest indirect dispatches:");
    for (const auto& l : largest) {
        if (l.groups != 0) {
            std::printf(" cs %016llx %ux%ux%u;", (unsigned long long)l.hash, l.dims[0], l.dims[1],
                        l.dims[2]);
        }
    }
    std::printf("\n");
}

} // namespace Vulkan::Breadcrumbs
