// SPDX-License-Identifier: GPL-3.0-or-later
// Windows probe for the PC memory model (step 6 of the direct path): can the GPU use the game's
// section-backed memory in place? Mirrors src/win32_memory.c: a pagefile section, views placed with
// MapViewOfFile3 into VirtualAlloc2 placeholders, a second (alias) view of the same pages.
// Build: clang++ -O2 -std=c++20 host_import_probe_win.cpp -o bb-host-import-probe.exe -lvulkan-1
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

typedef PVOID(WINAPI* VirtualAlloc2Fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
typedef PVOID(WINAPI* MapViewOfFile3Fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { std::printf("FAIL %s -> %d\n", #x, r_); return 1; } } while (0)

static uint32_t FindType(const VkPhysicalDeviceMemoryProperties& p, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < p.memoryTypeCount; ++i)
        if ((bits >> i & 1) && (p.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}

int main() {
    constexpr size_t Size = 64ull << 20;
    HMODULE k = GetModuleHandleW(L"kernelbase.dll");
    auto va2 = (VirtualAlloc2Fn)(void*)GetProcAddress(k, "VirtualAlloc2");
    auto mv3 = (MapViewOfFile3Fn)(void*)GetProcAddress(k, "MapViewOfFile3");
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)Size, nullptr);
    void* place = va2(GetCurrentProcess(), nullptr, Size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
    auto* guest = (uint8_t*)mv3(section, GetCurrentProcess(), place, 0, Size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
    auto* alias = (uint8_t*)MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, Size);
    if (!guest || !alias) { std::printf("FAIL mapping (%lu)\n", GetLastError()); return 1; }
    std::printf("guest view %p, alias view %p\n", guest, alias);

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "hostimport", 1, nullptr, 0, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app};
    VkInstance inst; CHECK(vkCreateInstance(&ici, nullptr, &inst));
    uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices(inst, &n, pds.data());
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    for (auto d : pds) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(d, &p); if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { pd = d; std::printf("GPU %s\n", p.deviceName); } }
    if (!pd) pd = pds[0];
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &hostp};
    vkGetPhysicalDeviceProperties2(pd, &p2);
    std::printf("minImportedHostPointerAlignment %llu\n", (unsigned long long)hostp.minImportedHostPointerAlignment);

    float prio = 1;
    uint32_t qf = 0;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, qf, 1, &prio};
    const char* exts[] = {VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME};
    VkPhysicalDeviceFeatures feats{}; feats.sparseBinding = VK_TRUE; feats.sparseResidencyBuffer = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &qci, 0, nullptr, 1, exts, &feats};
    VkDevice dev; CHECK(vkCreateDevice(pd, &dci, nullptr, &dev));
    auto getHostProps = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(dev, "vkGetMemoryHostPointerPropertiesEXT");
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    VkExternalMemoryHandleTypeFlagBits types[] = {VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                  VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_MAPPED_FOREIGN_MEMORY_BIT_EXT};
    const char* names[] = {"host allocation", "host mapped foreign memory"};
    int chosen = -1; uint32_t bits = 0;
    for (int t = 0; t < 2; ++t) {
        VkMemoryHostPointerPropertiesEXT hp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        VkResult r = getHostProps(dev, types[t], guest, &hp);
        std::printf("%s: result %d, memoryTypeBits 0x%x\n", names[t], r, hp.memoryTypeBits);
        if (r == VK_SUCCESS && hp.memoryTypeBits && chosen < 0) { chosen = t; bits = hp.memoryTypeBits; }
    }
    if (chosen < 0) { std::printf("RESULT: the driver does not import section views\n"); return 2; }
    const uint32_t type = FindType(mp, bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    std::printf("import as %s, memory type %u (flags 0x%x)\n", names[chosen], type, mp.memoryTypes[type].propertyFlags);

    // Import the guest view; a buffer over it.
    VkExternalMemoryBufferCreateInfo ebci{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, nullptr, (VkExternalMemoryHandleTypeFlags)types[chosen]};
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ebci, 0, Size,
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkBuffer gbuf; CHECK(vkCreateBuffer(dev, &bci, nullptr, &gbuf));
    VkImportMemoryHostPointerInfoEXT imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, nullptr, types[chosen], guest};
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, Size, type};
    VkDeviceMemory gmem; CHECK(vkAllocateMemory(dev, &mai, nullptr, &gmem));
    CHECK(vkBindBufferMemory(dev, gbuf, gmem, 0));

    // VRAM buffer for the copies and a timing baseline.
    VkBufferCreateInfo vbci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, Size,
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer vbuf, vbuf2; CHECK(vkCreateBuffer(dev, &vbci, nullptr, &vbuf)); CHECK(vkCreateBuffer(dev, &vbci, nullptr, &vbuf2));
    VkMemoryRequirements vr; vkGetBufferMemoryRequirements(dev, vbuf, &vr);
    VkMemoryAllocateInfo vmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, vr.size, FindType(mp, vr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory vmem, vmem2; CHECK(vkAllocateMemory(dev, &vmai, nullptr, &vmem)); CHECK(vkAllocateMemory(dev, &vmai, nullptr, &vmem2));
    CHECK(vkBindBufferMemory(dev, vbuf, vmem, 0)); CHECK(vkBindBufferMemory(dev, vbuf2, vmem2, 0));
    // Readback buffer.
    VkBuffer rbuf; CHECK(vkCreateBuffer(dev, &vbci, nullptr, &rbuf));
    VkMemoryRequirements rr; vkGetBufferMemoryRequirements(dev, rbuf, &rr);
    VkMemoryAllocateInfo rmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, rr.size, FindType(mp, rr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)};
    VkDeviceMemory rmem; CHECK(vkAllocateMemory(dev, &rmai, nullptr, &rmem)); CHECK(vkBindBufferMemory(dev, rbuf, rmem, 0));
    void* rmap; CHECK(vkMapMemory(dev, rmem, 0, Size, 0, &rmap));

    VkQueue q; vkGetDeviceQueue(dev, qf, 0, &q);
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qf};
    VkCommandPool pool; CHECK(vkCreateCommandPool(dev, &cpci, nullptr, &pool));
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence; CHECK(vkCreateFence(dev, &fci, nullptr, &fence));
    auto run = [&](auto&& record) -> double {
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cb, &bi);
        record();
        vkEndCommandBuffer(cb);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cb};
        const auto t0 = std::chrono::steady_clock::now();
        vkQueueSubmit(q, 1, &si, fence);
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &fence);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    };
    auto barrier = [&] {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    };
    VkBufferCopy whole{0, 0, Size};
    int failures = 0;

    // 1. CPU writes through the guest view, the GPU reads them.
    for (size_t i = 0; i < Size; i += 4) *(uint32_t*)(guest + i) = (uint32_t)(i * 2654435761u);
    run([&] { vkCmdCopyBuffer(cb, gbuf, rbuf, 1, &whole); });
    failures += std::memcmp(rmap, guest, Size) != 0;
    std::printf("1 GPU reads CPU writes: %s\n", std::memcmp(rmap, guest, Size) ? "MISMATCH" : "ok");
    // 2. CPU writes again through the ALIAS view: no re-import, no upload.
    for (size_t i = 0; i < Size; i += 4096) *(uint32_t*)(alias + i) = 0xC0FFEE00u + (uint32_t)(i >> 12);
    run([&] { vkCmdCopyBuffer(cb, gbuf, rbuf, 1, &whole); });
    bool ok2 = true;
    for (size_t i = 0; i < Size; i += 4096) ok2 &= ((uint32_t*)rmap)[i / 4] == 0xC0FFEE00u + (uint32_t)(i >> 12);
    failures += !ok2;
    std::printf("2 GPU sees later writes through the alias view: %s\n", ok2 ? "ok" : "MISMATCH");
    // 3. The GPU writes into the guest memory; the CPU reads them through both views.
    run([&] { vkCmdFillBuffer(cb, gbuf, 1 << 20, 1 << 20, 0x5A5A5A5Au); });
    bool ok3 = *(uint32_t*)(guest + (1 << 20) + 4096) == 0x5A5A5A5Au && *(uint32_t*)(alias + (2 << 20) - 4) == 0x5A5A5A5Au;
    failures += !ok3;
    std::printf("3 CPU sees GPU writes in both views: %s\n", ok3 ? "ok" : "MISMATCH");
    // 4. Read bandwidth: from guest memory (over PCIe) vs from VRAM.
    run([&] { vkCmdCopyBuffer(cb, gbuf, vbuf, 1, &whole); });
    double host_ms = 0, vram_ms = 0;
    for (int i = 0; i < 5; ++i) host_ms += run([&] { vkCmdCopyBuffer(cb, gbuf, vbuf, 1, &whole); });
    for (int i = 0; i < 5; ++i) vram_ms += run([&] { vkCmdCopyBuffer(cb, vbuf, vbuf2, 1, &whole); barrier(); });
    std::printf("4 64 MiB GPU copy: from guest memory %.2f ms (%.1f GB/s), VRAM to VRAM %.2f ms\n",
                host_ms / 5, 64.0 / 1024 / (host_ms / 5 / 1000), vram_ms / 5);
    // 5. An import of a 4 KiB-aligned piece inside the view (the engine maps per allocation).
    VkMemoryAllocateInfo part{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, 1 << 20, type};
    imp.pHostPointer = guest + (8 << 20) + 4096 * 3;
    VkDeviceMemory pmem; VkResult pr = vkAllocateMemory(dev, &part, nullptr, &pmem);
    std::printf("5 import at a 4 KiB offset: %d\n", pr);
    // 6. The engine's arena: a sparse buffer whose blocks are bound to guest memory (BindInPlace).
    {
        constexpr VkDeviceSize ArenaSize = 1ull << 30;
        VkBufferCreateInfo sci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ebci,
                               VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT, ArenaSize,
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
        VkBuffer arena; VkResult ar = vkCreateBuffer(dev, &sci, nullptr, &arena);
        std::printf("6a sparse arena with the host import handle type: %d\n", ar);
        if (ar != VK_SUCCESS) {
            sci.pNext = nullptr;
            ar = vkCreateBuffer(dev, &sci, nullptr, &arena);
            std::printf("6a sparse arena without it: %d\n", ar);
        }
        VkMemoryRequirements sr; vkGetBufferMemoryRequirements(dev, arena, &sr);
        std::printf("6b sparse block alignment %llu, memory types 0x%x (import type %u %s)\n",
                    (unsigned long long)sr.alignment, sr.memoryTypeBits, type,
                    (sr.memoryTypeBits >> type & 1) ? "allowed" : "NOT allowed");
        VkSparseMemoryBind mb{64ull << 20, 16ull << 20, gmem, 4ull << 20, 0};
        VkSparseBufferMemoryBindInfo bbi{arena, 1, &mb};
        VkBindSparseInfo bsi{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, nullptr, 0, nullptr, 1, &bbi};
        VkResult br = vkQueueBindSparse(q, 1, &bsi, fence);
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &fence);
        std::printf("6c sparse bind of imported memory: %d\n", br);
        for (size_t i = 0; i < (16u << 20); i += 4) *(uint32_t*)(guest + (4u << 20) + i) = 0xA5000000u ^ (uint32_t)i;
        VkBufferCopy c{64ull << 20, 0, 16ull << 20};
        run([&] { vkCmdCopyBuffer(cb, arena, rbuf, 1, &c); });
        const bool ok6 = br == VK_SUCCESS && std::memcmp(rmap, guest + (4u << 20), 16u << 20) == 0;
        failures += !ok6;
        std::printf("6d GPU reads guest memory through the sparse arena: %s\n", ok6 ? "ok" : "MISMATCH");
    }
    std::printf("RESULT: %s\n", failures ? "imported, but wrong data" : "the GPU can use the game's memory in place");
    return failures ? 3 : 0;
}
