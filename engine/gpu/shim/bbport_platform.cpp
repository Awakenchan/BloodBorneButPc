// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include <windows.h>
#include <psapi.h>
#include "bbport_platform.h"
#include "bbport_threads.h"

namespace BbPlatform {

static std::uint64_t FileTimeUs(const FILETIME& t) {
    return ((std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime) / 10;
}

bool GetUsage(bool thread, Usage& out) {
    FILETIME creation, exit, kernel, user;
    const BOOL ok = thread ? GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)
                           : GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
    if (!ok) {
        return false;
    }
    out = {};
    out.user_us = FileTimeUs(user);
    out.sys_us = FileTimeUs(kernel);
    if (!thread) {
        PROCESS_MEMORY_COUNTERS counters{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
            out.minor_faults = counters.PageFaultCount;
        }
    }
    return true;
}

bool GetMemory(Memory& out) {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof(counters)) ||
        !GlobalMemoryStatusEx(&status)) {
        return false;
    }
    out.private_bytes = counters.PrivateUsage;
    out.working_set = counters.WorkingSetSize;
    out.peak_working_set = counters.PeakWorkingSetSize;
    out.free_phys = status.ullAvailPhys;
    out.total_phys = status.ullTotalPhys;
    out.commit_left = status.ullAvailPageFile;
    return true;
}

bool GetMemoryBreakdown(MemoryBreakdown& out) {
    std::vector<ULONG_PTR> buffer(1 << 20);
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (QueryWorkingSet(GetCurrentProcess(), buffer.data(), DWORD(buffer.size() * sizeof(ULONG_PTR)))) break;
        if (GetLastError() != ERROR_BAD_LENGTH) return false;
        buffer.resize(buffer.front() + 4096 + buffer.size());
        if (attempt == 3) return false;
    }
    const std::size_t count = std::min<std::size_t>(buffer.front(), buffer.size() - 1);
    std::vector<std::uint64_t> pages;
    pages.reserve(count);
    std::uint64_t shared = 0;
    for (std::size_t i = 1; i <= count; ++i) {
        const auto& info = reinterpret_cast<const PSAPI_WORKING_SET_BLOCK&>(buffer[i]);
        pages.push_back(std::uint64_t(info.VirtualPage) << 12);
        shared += info.Shared ? 4096 : 0;
    }
    std::sort(pages.begin(), pages.end());
    constexpr std::uint64_t kGameMin = 0x1000000000ull, kGameMax = 0x1000000000ull * 16;
    MEMORY_BASIC_INFORMATION region{};
    std::uint64_t region_end = 0;
    for (const std::uint64_t page : pages) {
        if (page >= region_end) {
            if (!VirtualQuery(reinterpret_cast<void*>(page), &region, sizeof region)) continue;
            region_end = std::uint64_t(region.BaseAddress) + region.RegionSize;
        }
        if (region.Type == MEM_IMAGE) out.images += 4096;
        else if (region.Type == MEM_MAPPED) (page >= kGameMin && page < kGameMax ? out.game_pool : out.mapped) += 4096;
        else out.priv += 4096;
    }
    out.shared = shared;
    return true;
}

std::uint32_t CurrentThreadId() {
    return static_cast<std::uint32_t>(GetCurrentThreadId());
}

int ThreadCpuClock() {
    HANDLE handle = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                         THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
        return -1;
    }
    return static_cast<int>(reinterpret_cast<std::intptr_t>(handle));
}

std::uint64_t ReadThreadCpuClockNs(int clock) {
    FILETIME creation, exit, kernel, user;
    const HANDLE handle = reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(clock));
    if (!GetThreadTimes(handle, &creation, &exit, &kernel, &user)) {
        return 0;
    }
    return (FileTimeUs(kernel) + FileTimeUs(user)) * 1000;
}

void DescribeAddress(const void* address, char* out, std::size_t size) {
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module) &&
        GetModuleFileNameA(module, name, sizeof(name))) {
        std::snprintf(out, size, "%s+0x%llx", name,
                      static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(address) -
                                                      reinterpret_cast<std::uintptr_t>(module)));
        return;
    }
    std::snprintf(out, size, "%p", address);
}

std::size_t CaptureStack(void** frames, std::size_t count) {
    return CaptureStackBackTrace(0, static_cast<DWORD>(count), frames, nullptr);
}

bool ReadProcessMemory(const void* source, void* destination, std::size_t size) {
    SIZE_T read = 0;
    return ::ReadProcessMemory(GetCurrentProcess(), source, destination, size, &read) && read == size;
}

void TerminateProcessNow(unsigned code) {
    std::fflush(nullptr);
    TerminateProcess(GetCurrentProcess(), code);
    _exit(static_cast<int>(code));
}

}

namespace BbThreads {

unsigned Available() {
    DWORD_PTR process = 0, system = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process) {
        return std::max(1, std::popcount(static_cast<unsigned long long>(process)));
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

void MakeBackground() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
}

namespace {
bool Raising() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_GPU_THREAD_PRIORITY");
        return !(env && env[0] == '0');
    }();
    return enabled;
}
}

void MakeCritical(bool bulk) {
    if (Raising() && (!bulk || Available() >= 16)) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    }
}

unsigned CapSpinUs(unsigned wanted) {
    return Raising() && Available() < 16 ? std::min(wanted, 50u) : wanted;
}

}
#endif
