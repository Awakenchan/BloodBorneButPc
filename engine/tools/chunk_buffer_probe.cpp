// SPDX-License-Identifier: GPL-3.0-or-later
// The Vulkan calls the PC memory model makes for a guest memory chunk on Windows (BbGuestMemory,
// step 6 item 2 of docs/EMULATION_REMOVAL_PLAN.md), alone, so the validation layer can judge them
// on any driver, lavapipe in WSL included (the engine on Linux makes dma-buf chunks instead):
//   chunk        host memory imported (VK_EXT_external_memory_host) with the chunk's own buffer;
//   beta 7       the buffer cache's buffer over it as 0.9.37 beta 7 made it (dma-buf handle type);
//   chunk route  that buffer as BB_CHUNK_BUFFERS makes it: the import's handle type and usage,
//                bound as vertex and index buffer, copied through, written by the GPU;
//   arena        the import bound into a sparse buffer as BindInPlace does (outside the spec).
// Build: clang++ -O2 -std=c++20 chunk_buffer_probe.cpp -o bb-chunk-buffer-probe -lvulkan
//        (Windows: -lvulkan-1)
// Run:   VK_LOADER_LAYERS_ENABLE='*validation' ./bb-chunk-buffer-probe
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace {
const char* phase = "setup";
std::map<std::string, int> errors;      // phase -> validation errors
std::map<std::string, std::string> ids; // phase -> message ids

VKAPI_ATTR VkBool32 VKAPI_CALL OnMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                         VkDebugUtilsMessageTypeFlagsEXT,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++errors[phase];
        const std::string id = data->pMessageIdName ? data->pMessageIdName : "?";
        if (ids[phase].find(id) == std::string::npos) {
            ids[phase] += (ids[phase].empty() ? "" : ", ") + id;
        }
    }
    return VK_FALSE;
}

uint32_t FindType(const VkPhysicalDeviceMemoryProperties& props, uint32_t bits,
                  VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((bits >> i & 1) && (props.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return ~0u;
}

bool HasExtension(VkPhysicalDevice device, const char* name) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, exts.data());
    for (const auto& ext : exts) {
        if (std::strcmp(ext.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

void* AllocHost(size_t size) {
#ifdef _WIN32
    return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}
} // namespace

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        const VkResult r_ = (x);                                                                   \
        if (r_ != VK_SUCCESS) {                                                                    \
            std::printf("FAIL %s -> %d\n", #x, r_);                                                \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main() {
    constexpr VkDeviceSize ChunkSize = 8ull << 20;
    // BbGuestMemory::ChunkUsage
    constexpr VkBufferUsageFlags ChunkUsage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

    const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "chunk-probe", 1,
                                nullptr, 0, VK_API_VERSION_1_3};
    const char* instance_exts[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app, 0, nullptr,
                             1, instance_exts};
    VkInstance inst;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) {
        ici.enabledExtensionCount = 0; // no debug utils: no validation counts either
        CHECK(vkCreateInstance(&ici, nullptr, &inst));
        std::printf("no VK_EXT_debug_utils: the validation layer's verdict is not counted\n");
    } else {
        const VkDebugUtilsMessengerCreateInfoEXT mci{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT, nullptr, 0,
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
            &OnMessage, nullptr};
        auto create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerEXT messenger;
        if (create) {
            create(inst, &mci, nullptr, &messenger);
        }
    }
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(inst, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(inst, &count, devices.data());
    if (devices.empty()) {
        std::printf("FAIL no Vulkan device\n");
        return 1;
    }
    VkPhysicalDevice pd = devices[0];
    for (auto d : devices) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            pd = d;
        }
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    std::printf("GPU %s, driver %u\n", props.deviceName, props.driverVersion);
    if (!HasExtension(pd, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) {
        std::printf("RESULT: no VK_EXT_external_memory_host on this driver\n");
        return 2;
    }
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(pd, &supported);
    const bool sparse = supported.sparseBinding && supported.sparseResidencyBuffer;
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_props{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &host_props};
    vkGetPhysicalDeviceProperties2(pd, &props2);

    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &families, nullptr);
    std::vector<VkQueueFamilyProperties> family_props(families);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &families, family_props.data());
    uint32_t qf = 0;
    for (uint32_t i = 0; i < families; ++i) {
        const VkQueueFlags want =
            VK_QUEUE_GRAPHICS_BIT | (sparse ? VK_QUEUE_SPARSE_BINDING_BIT : VkQueueFlags{0});
        if ((family_props[i].queueFlags & want) == want) {
            qf = i;
            break;
        }
    }
    const float priority = 1;
    const VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, qf, 1,
                                      &priority};
    const char* device_exts[] = {VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME};
    VkPhysicalDeviceFeatures features{};
    features.sparseBinding = sparse;
    features.sparseResidencyBuffer = sparse;
    const VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &qci, 0,
                                 nullptr, 1, device_exts, &features};
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &dci, nullptr, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, qf, 0, &queue);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    auto host_pointer_props = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(
        dev, "vkGetMemoryHostPointerPropertiesEXT");

    // A chunk: host memory imported, with its own buffer (bbport_guest_memory_win.cpp Import).
    phase = "chunk";
    auto* host = static_cast<uint8_t*>(AllocHost(ChunkSize));
    if (!host || reinterpret_cast<uintptr_t>(host) % host_props.minImportedHostPointerAlignment) {
        std::printf("FAIL host memory (alignment %llu)\n",
                    (unsigned long long)host_props.minImportedHostPointerAlignment);
        return 1;
    }
    VkMemoryHostPointerPropertiesEXT pointer_props{
        VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    CHECK(host_pointer_props(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host,
                             &pointer_props));
    const uint32_t type =
        FindType(mp, pointer_props.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    const VkExternalMemoryBufferCreateInfo host_external{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, nullptr,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &host_external, 0, ChunkSize,
                           ChunkUsage, VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer chunk_buffer;
    CHECK(vkCreateBuffer(dev, &bci, nullptr, &chunk_buffer));
    const VkImportMemoryHostPointerInfoEXT import_info{
        VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, nullptr,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host};
    const VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import_info, ChunkSize,
                                   type};
    VkDeviceMemory chunk_memory;
    CHECK(vkAllocateMemory(dev, &mai, nullptr, &chunk_memory));
    CHECK(vkBindBufferMemory(dev, chunk_buffer, chunk_memory, 0));
    std::printf("chunk: %llu MiB imported, memory type %u (host pointer types 0x%x)\n",
                (unsigned long long)(ChunkSize >> 20), type, pointer_props.memoryTypeBits);

    // The buffer cache's buffer over it, as beta 7 made it (Buffer's chunk constructor).
    phase = "beta 7";
    {
        const VkExternalMemoryBufferCreateInfo dma_buf{
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, nullptr,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkBufferCreateInfo old_ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &dma_buf, 0, ChunkSize,
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                                      VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT,
                                  VK_SHARING_MODE_EXCLUSIVE};
        VkBuffer old_buffer;
        if (vkCreateBuffer(dev, &old_ci, nullptr, &old_buffer) == VK_SUCCESS) {
            vkBindBufferMemory(dev, old_buffer, chunk_memory, 0);
            vkDestroyBuffer(dev, old_buffer, nullptr);
        }
    }

    // As BB_CHUNK_BUFFERS makes it, and used as the chunk route uses it.
    phase = "chunk route";
    VkBuffer route;
    CHECK(vkCreateBuffer(dev, &bci, nullptr, &route));
    VkMemoryRequirements route_req;
    vkGetBufferMemoryRequirements(dev, route, &route_req);
    std::printf("chunk route buffer: memory types 0x%x (import type %u %s)\n",
                route_req.memoryTypeBits, type,
                (route_req.memoryTypeBits >> type & 1) ? "allowed" : "NOT allowed");
    CHECK(vkBindBufferMemory(dev, route, chunk_memory, 0));

    const VkBufferCreateInfo rbci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, ChunkSize,
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer readback;
    CHECK(vkCreateBuffer(dev, &rbci, nullptr, &readback));
    VkMemoryRequirements rr;
    vkGetBufferMemoryRequirements(dev, readback, &rr);
    const VkMemoryAllocateInfo rmai{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, rr.size,
        FindType(mp, rr.memoryTypeBits,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory readback_memory;
    CHECK(vkAllocateMemory(dev, &rmai, nullptr, &readback_memory));
    CHECK(vkBindBufferMemory(dev, readback, readback_memory, 0));
    void* readback_map;
    CHECK(vkMapMemory(dev, readback_memory, 0, ChunkSize, 0, &readback_map));

    const VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                       VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qf};
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(dev, &cpci, nullptr, &pool));
    const VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
                                           pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));
    const VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(vkCreateFence(dev, &fci, nullptr, &fence));
    const auto run = [&](auto&& record) {
        vkResetCommandBuffer(cb, 0);
        const VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                          VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cb, &bi);
        record();
        const VkMemoryBarrier to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                             &to_host, 0, nullptr, 0, nullptr);
        vkEndCommandBuffer(cb);
        const VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cb};
        vkQueueSubmit(queue, 1, &si, fence);
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &fence);
    };
    int failures = 0;
    // The CPU writes the game's memory; the GPU reads it through the route (bound as a vertex
    // and an index buffer too, as draws bind it).
    for (size_t i = 0; i < ChunkSize; i += 4) {
        *reinterpret_cast<uint32_t*>(host + i) = static_cast<uint32_t>(i * 2654435761u);
    }
    const VkBufferCopy whole{0, 0, ChunkSize};
    run([&] {
        const VkDeviceSize offset = 4096;
        vkCmdBindVertexBuffers(cb, 0, 1, &route, &offset);
        vkCmdBindIndexBuffer(cb, route, offset, VK_INDEX_TYPE_UINT32);
        vkCmdCopyBuffer(cb, route, readback, 1, &whole);
    });
    const bool reads = std::memcmp(readback_map, host, ChunkSize) == 0;
    failures += !reads;
    std::printf("chunk route: the GPU reads the CPU's bytes: %s\n", reads ? "ok" : "MISMATCH");
    // The GPU writes through the route (a command processor write, a shader's); the CPU sees it.
    run([&] { vkCmdFillBuffer(cb, route, 1 << 20, 1 << 20, 0x5A5A5A5Au); });
    const bool writes = *reinterpret_cast<uint32_t*>(host + (1 << 20) + 4096) == 0x5A5A5A5Au;
    failures += !writes;
    std::printf("chunk route: the CPU sees the GPU's writes: %s\n", writes ? "ok" : "MISMATCH");

    // The arena's way: the import bound into a sparse buffer (BindInPlace).
    phase = "arena";
    if (!sparse) {
        std::printf("arena: no sparse binding on this driver, left out\n");
    } else {
        constexpr VkDeviceSize ArenaSize = 64ull << 20;
        const VkBufferCreateInfo sci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr,
                                     VK_BUFFER_CREATE_SPARSE_BINDING_BIT |
                                         VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT,
                                     ArenaSize, ChunkUsage, VK_SHARING_MODE_EXCLUSIVE};
        VkBuffer arena;
        CHECK(vkCreateBuffer(dev, &sci, nullptr, &arena));
        VkMemoryRequirements sr;
        vkGetBufferMemoryRequirements(dev, arena, &sr);
        std::printf("arena: blocks of %llu KiB, memory types 0x%x (import type %u %s)\n",
                    (unsigned long long)(sr.alignment >> 10), sr.memoryTypeBits, type,
                    (sr.memoryTypeBits >> type & 1) ? "allowed" : "NOT allowed");
        const VkSparseMemoryBind bind{16ull << 20, 4ull << 20, chunk_memory, 2ull << 20, 0};
        const VkSparseBufferMemoryBindInfo buffer_bind{arena, 1, &bind};
        const VkBindSparseInfo bsi{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, nullptr, 0, nullptr, 1,
                                   &buffer_bind};
        const VkResult bound = vkQueueBindSparse(queue, 1, &bsi, fence);
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &fence);
        const VkBufferCopy part{16ull << 20, 0, 4ull << 20};
        run([&] { vkCmdCopyBuffer(cb, arena, readback, 1, &part); });
        const bool same = bound == VK_SUCCESS &&
                          std::memcmp(readback_map, host + (2u << 20), 4u << 20) == 0;
        std::printf("arena: bound %d, the GPU reads the game's bytes through it: %s\n", bound,
                    same ? "ok" : "MISMATCH");
    }

    std::printf("validation errors:\n");
    for (const char* name : {"setup", "chunk", "beta 7", "chunk route", "arena"}) {
        std::printf("  %-12s %d%s%s\n", name, errors[name], ids[name].empty() ? "" : "  ",
                    ids[name].c_str());
    }
    const bool route_valid = errors["chunk"] == 0 && errors["chunk route"] == 0;
    std::printf("RESULT: the chunk route %s, %s\n", route_valid ? "is valid" : "has VALIDATION ERRORS",
                failures ? "with wrong data" : "the data right");
    return route_valid && !failures ? 0 : 3;
}
