// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef _WIN32
#include "bbport_guest_hooks.h"
#include "bbport_heap_sites.h"
#include "bbport_toggles.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

extern "C" void runtime_memory_note_write(uintptr_t address, uint64_t size);

namespace BbGuestHooks {
namespace {
using u64 = std::uint64_t;
constexpr u64 ImageBase = 0x800000000ull;

constexpr u64 AllocReturn = 0x26aa255;
constexpr unsigned char AllocReturnCode[4] = {0x48, 0x83, 0xc4, 0x18};
RangeCallback on_range_allocated = nullptr;
constexpr std::array<u64, 3> HeapAllocReturns = {0x11e6629, 0x11e6736, 0x11e6846};
constexpr unsigned char HeapAllocReturnCode[3] = {0x48, 0x89, 0xc3};
constexpr u64 CollectorEntry = 0x26aa860;
constexpr unsigned char CollectorEntryCode[1] = {0x55};
constexpr u64 ReleaseCheck = 0xbb5fb0;
constexpr unsigned char ReleaseCheckCode[2] = {0x78, 0x15};
struct CallSite {
    u64 offset;
    unsigned char code[5];
};
constexpr std::array<CallSite, 2> MemcpySites = {{
    {0x216d5ea, {0xe8, 0xc1, 0xcb, 0x4a, 0x00}},
    {0x2171fea, {0xe8, 0xc1, 0x81, 0x4a, 0x00}},
}};
std::array<std::atomic<u64>, MemcpySites.size()> hits{};

__attribute__((sysv_abi)) void* LoaderCopy(void* dst, const void* src, std::size_t size) {
    std::memcpy(dst, src, size);
    runtime_memory_note_write(reinterpret_cast<u64>(dst), size);
    return dst;
}

LONG CALLBACK OnBreakpoint(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT& c = *info->ContextRecord;
    const u64 rip = reinterpret_cast<u64>(info->ExceptionRecord->ExceptionAddress);
    if (rip < ImageBase || rip >= ImageBase + 0x10000000) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (rip == ImageBase + ReleaseCheck) {
        const auto result = static_cast<std::int32_t>(c.Rax & 0xffffffff);
        BbHeapSites::NoteReleaseCheck(std::uint32_t(result));
        c.Rip = ImageBase + (result < 0 ? 0xbb5fc7 : ReleaseCheck + 2);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (rip == ImageBase + CollectorEntry) {
        const u64 allocator = c.Rdi;
        for (std::size_t i = 0; i < BbStats::range_allocators.size(); ++i) {
            u64 expected = 0;
            if (BbStats::range_allocators[i].load(std::memory_order_relaxed) == allocator ||
                BbStats::range_allocators[i].compare_exchange_strong(expected, allocator)) {
                BbStats::range_live[i].store(*reinterpret_cast<const u64*>(allocator + 0xc8),
                                             std::memory_order_relaxed);
                BbStats::range_bytes[i].store(*reinterpret_cast<const u64*>(allocator + 0xd0),
                                              std::memory_order_relaxed);
                break;
            }
        }
        c.Rsp -= 8;
        *reinterpret_cast<u64*>(c.Rsp) = c.Rbp;
        c.Rip = ImageBase + CollectorEntry + 1;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    for (const u64 site : HeapAllocReturns) {
        if (rip != ImageBase + site) {
            continue;
        }
        const u64 address = c.Rax;
        const u64 size = *reinterpret_cast<const u64*>(c.Rbp - 0x50);
        if (address != 0 && size != 0 && size < (u64(1) << 32) && on_range_allocated) {
            on_range_allocated(address, size);
        }
        c.Rbx = c.Rax;
        c.Rip = ImageBase + site + sizeof(HeapAllocReturnCode);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (rip == ImageBase + AllocReturn) {
        if (const u64 node = c.Rax; node && on_range_allocated) {
            const u64 start = *reinterpret_cast<const u64*>(node + 8);
            const u64 next = *reinterpret_cast<const u64*>(node + 0x18);
            const u64 end = next ? *reinterpret_cast<const u64*>(next + 8) : start;
            if (end > start && end - start < (u64(1) << 32)) {
                on_range_allocated(start, end - start);
            }
        }
        c.Rsp += 0x18;
        c.Rip = ImageBase + AllocReturn + sizeof(AllocReturnCode);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    for (std::size_t i = 0; i < MemcpySites.size(); ++i) {
        const auto& site = MemcpySites[i];
        if (rip != ImageBase + site.offset) {
            continue;
        }
        hits[i].fetch_add(1, std::memory_order_relaxed);
        c.Rsp -= 8;
        *reinterpret_cast<u64*>(c.Rsp) = ImageBase + site.offset + 5;
        c.Rip = reinterpret_cast<u64>(&LoaderCopy);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

bool Patch(u64 address, const unsigned char* expected, std::size_t size) {
    auto* code = reinterpret_cast<unsigned char*>(address);
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(code, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
        std::memcmp(code, expected, size) != 0) {
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(code, size, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    __atomic_store_n(code, static_cast<unsigned char>(0xcc), __ATOMIC_SEQ_CST);
    VirtualProtect(code, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), code, size);
    return true;
}
}

void Install(RangeCallback on_gpu_range_allocated) {
    static std::atomic<bool> installed{false};
    if (installed.exchange(true)) {
        return;
    }
    on_range_allocated = on_gpu_range_allocated;
    AddVectoredExceptionHandler(1, &OnBreakpoint);
    int patched = 0;
    for (const auto& site : MemcpySites) {
        patched += Patch(ImageBase + site.offset, site.code, sizeof(site.code));
    }
    std::printf("Guest hooks: %d of %zu resource copy sites tell the GPU side of their writes\n",
                patched, MemcpySites.size());
    const bool allocator = Patch(ImageBase + AllocReturn, AllocReturnCode, sizeof(AllocReturnCode));
    const char* heap_env = std::getenv("BB_HEAP_HOOKS");
    const bool heap_hooks = !heap_env || heap_env[0] != '0';
    if (BbStats::enabled) {
        Patch(ImageBase + CollectorEntry, CollectorEntryCode, sizeof(CollectorEntryCode));
    }
    int heap_sites = 0;
    for (const u64 site : HeapAllocReturns) {
        if (!heap_hooks) {
            break;
        }
        heap_sites += Patch(ImageBase + site, HeapAllocReturnCode, sizeof(HeapAllocReturnCode));
    }
    std::printf("Guest hooks: GPU heap allocations seen at %d of %zu call sites\n", heap_sites,
                HeapAllocReturns.size());
    std::printf("Guest hooks: GPU memory allocator %s\n",
                allocator ? "tells the GPU side of new ranges" : "not found");
    BbHeapSites::Install();
    if (const char* env = std::getenv("BB_HEAP_SITES"); env && env[0] == '1') {
        Patch(ImageBase + ReleaseCheck, ReleaseCheckCode, sizeof(ReleaseCheckCode));
    }
}
}
#endif
