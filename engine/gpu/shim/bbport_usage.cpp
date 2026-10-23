// SPDX-License-Identifier: GPL-3.0-or-later
#include "bbport_usage.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <tlhelp32.h>
#endif

namespace BbUsage {

#ifdef _WIN32
namespace {

std::uint64_t Ticks(const FILETIME& t) {
    return (std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}

float CpuPercent() {
    static std::uint64_t last_idle = 0, last_total = 0;
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) {
        return -1.0f;
    }
    const std::uint64_t i = Ticks(idle), total = Ticks(kernel) + Ticks(user);
    const std::uint64_t di = i - last_idle, dt = total - last_total;
    const bool first = last_total == 0;
    last_idle = i;
    last_total = total;
    if (first || dt == 0) {
        return -1.0f;
    }
    return std::clamp(100.0f * float(dt - std::min(di, dt)) / float(dt), 0.0f, 100.0f);
}

class GpuCounter {
public:
    float cpu = -1.0f;

    float Percent() {
        cpu = -1.0f;
        if (!Open()) {
            return -1.0f;
        }
        if (collect(query) != ERROR_SUCCESS) {
            return -1.0f;
        }
        PDH_FMT_COUNTERVALUE one{};
        if (cpu_counter && value && value(cpu_counter, PDH_FMT_DOUBLE, nullptr, &one) == ERROR_SUCCESS &&
            one.CStatus == ERROR_SUCCESS) {
            cpu = float(std::clamp(one.doubleValue, 0.0, 100.0));
        }
        DWORD bytes = 0, count = 0;
        PDH_STATUS status = array(counter, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
        if (status != PDH_MORE_DATA || bytes == 0) {
            return -1.0f;
        }
        buffer.resize(bytes);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (array(counter, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) {
            return -1.0f;
        }
        double sum = 0.0;
        for (DWORD k = 0; k < count; ++k) {
            if (items[k].FmtValue.CStatus == ERROR_SUCCESS) {
                sum += items[k].FmtValue.doubleValue;
            }
        }
        return float(std::clamp(sum, 0.0, 100.0));
    }

private:
    bool Open() {
        if (tried) {
            return query != nullptr;
        }
        tried = true;
        HMODULE pdh = LoadLibraryW(L"pdh.dll");
        if (!pdh) {
            return false;
        }
        const auto open = reinterpret_cast<decltype(&PdhOpenQueryW)>(GetProcAddress(pdh, "PdhOpenQueryW"));
        const auto add = reinterpret_cast<decltype(&PdhAddEnglishCounterW)>(
            GetProcAddress(pdh, "PdhAddEnglishCounterW"));
        collect = reinterpret_cast<decltype(&PdhCollectQueryData)>(GetProcAddress(pdh, "PdhCollectQueryData"));
        array = reinterpret_cast<decltype(&PdhGetFormattedCounterArrayW)>(
            GetProcAddress(pdh, "PdhGetFormattedCounterArrayW"));
        value = reinterpret_cast<decltype(&PdhGetFormattedCounterValue)>(
            GetProcAddress(pdh, "PdhGetFormattedCounterValue"));
        if (!open || !add || !collect || !array) {
            return false;
        }
        PDH_HQUERY q = nullptr;
        if (open(nullptr, 0, &q) != ERROR_SUCCESS) {
            return false;
        }
        if (add(q, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &counter) != ERROR_SUCCESS) {
            return false;
        }
        if (add(q, L"\\Processor Information(_Total)\\% Processor Utility", 0, &cpu_counter) != ERROR_SUCCESS) {
            cpu_counter = nullptr;
        }
        query = q;
        collect(query);
        return true;
    }

    bool tried = false;
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr, cpu_counter = nullptr;
    decltype(&PdhCollectQueryData) collect = nullptr;
    decltype(&PdhGetFormattedCounterArrayW) array = nullptr;
    decltype(&PdhGetFormattedCounterValue) value = nullptr;
    std::vector<unsigned char> buffer;
};

void ThreadReportLoop() {
    using Describe = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    const auto describe = reinterpret_cast<Describe>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
    std::vector<std::pair<DWORD, std::uint64_t>> last;
    auto previous = std::chrono::steady_clock::now();
    for (;;) {
        Sleep(5000);
        const auto now = std::chrono::steady_clock::now();
        const double wall = std::chrono::duration<double>(now - previous).count() * 1e7;
        previous = now;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) continue;
        struct Row { double percent; std::wstring name; DWORD id; };
        std::vector<Row> rows;
        std::vector<std::pair<DWORD, std::uint64_t>> current;
        double total = 0.0;
        THREADENTRY32 entry{sizeof(entry)};
        for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != GetCurrentProcessId()) continue;
            HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
            if (!thread) continue;
            FILETIME created, exited, kernel, user;
            if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
                const std::uint64_t time = Ticks(kernel) + Ticks(user);
                current.emplace_back(entry.th32ThreadID, time);
                const auto it = std::find_if(last.begin(), last.end(),
                                             [&](const auto& p) { return p.first == entry.th32ThreadID; });
                if (it != last.end() && wall > 0) {
                    const double percent = 100.0 * double(time - it->second) / wall;
                    total += percent;
                    std::wstring name;
                    PWSTR text = nullptr;
                    if (describe && SUCCEEDED(describe(thread, &text)) && text) {
                        name = text;
                        LocalFree(text);
                    }
                    rows.push_back({percent, name, entry.th32ThreadID});
                }
            }
            CloseHandle(thread);
        }
        CloseHandle(snapshot);
        last = std::move(current);
        if (rows.empty()) continue;
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.percent > b.percent; });
        std::printf("Thread CPU (5 s, 100%% = one core): all %.0f%%;", total);
        for (std::size_t k = 0; k < rows.size() && k < 12; ++k) {
            std::printf(" %ls %.0f%%%s", rows[k].name.empty() ? L"(unnamed)" : rows[k].name.c_str(),
                        rows[k].percent, k + 1 < rows.size() && k < 11 ? "," : "");
        }
        std::printf("\n");
        std::fflush(stdout);
    }
}

const bool thread_report = [] {
    const char* env = std::getenv("BB_THREAD_CPU");
    if (env && env[0] == '1') {
        std::thread(ThreadReportLoop).detach();
    }
    return true;
}();

}

Sample Get(bool cpu, bool gpu) {
    static Sample last;
    static auto next = std::chrono::steady_clock::time_point{};
    static GpuCounter counter;
    const auto now = std::chrono::steady_clock::now();
    if (now < next) {
        return last;
    }
    next = now + std::chrono::milliseconds(500);
    static const bool utility = [] {
        const char* env = std::getenv("BB_CPU_UTILITY");
        return env && env[0] == '1';
    }();
    const float gpu_now = counter.Percent();
    const float system_cpu = CpuPercent();
    if (cpu) {
        last.cpu = utility && counter.cpu >= 0.0f ? counter.cpu : system_cpu;
    }
    if (gpu) {
        last.gpu = gpu_now;
    }
    return last;
}
#else
Sample Get(bool, bool) {
    return {};
}
#endif

}
