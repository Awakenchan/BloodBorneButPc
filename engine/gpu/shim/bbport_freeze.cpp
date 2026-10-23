// SPDX-License-Identifier: GPL-3.0-or-later
#include "bbport_freeze.h"
#include "bbport_toggles.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" void runtime_blocked_report(void);

namespace BbFreeze {
namespace {
std::atomic<long long> last_flip_ms{0};
std::once_flag started;
std::mutex reporter_mutex;
std::function<void()> state_reporter;

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string Describe(HANDLE thread) {
    PWSTR wide = nullptr;
    std::string name;
    if (SUCCEEDED(GetThreadDescription(thread, &wide)) && wide) {
        char text[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, sizeof(text) - 1, nullptr, nullptr);
        name = text;
        LocalFree(wide);
    }
    return name;
}

std::string Module(DWORD64 address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module) ||
        !module) {
        char text[32];
        std::snprintf(text, sizeof(text), "%#llx", static_cast<unsigned long long>(address));
        return text;
    }
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* base = wcsrchr(path, L'\\');
    char name[MAX_PATH] = {};
    WideCharToMultiByte(CP_UTF8, 0, base ? base + 1 : path, -1, name, sizeof(name) - 1, nullptr,
                        nullptr);
    char text[MAX_PATH + 32];
    std::snprintf(text, sizeof(text), "%s+%#llx", name,
                  static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
    return text;
}

constexpr int kMaxFrames = 24;

int Walk(HANDLE thread, DWORD64 (&frames)[kMaxFrames]) {
    if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
        return 0;
    }
    int count = 0;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(thread, &context)) {
        while (count < kMaxFrames && context.Rip) {
            frames[count++] = context.Rip;
            DWORD64 image_base = 0;
            const PRUNTIME_FUNCTION function =
                RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
            if (!function) {
                if (count > 1 || !image_base) {
                    break;
                }
                context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
                context.Rsp += 8;
                continue;
            }
            void* handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                             &handler_data, &establisher, nullptr);
        }
    }
    ResumeThread(thread);
    return count;
}

void Report(long long frozen_ms) {
    std::printf("== Freeze: no frame for %.1f s; the threads' stacks and the GPU's state follow "
                "(BB_FREEZE_REPORT_S=0 turns this off)\n",
                frozen_ms / 1000.0);
    {
        std::scoped_lock lock{reporter_mutex};
        if (state_reporter) {
            state_reporter();
        }
    }
    runtime_blocked_report();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    std::vector<DWORD> ids;
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID == pid && entry.th32ThreadID != self) {
            ids.push_back(entry.th32ThreadID);
        }
    }
    CloseHandle(snapshot);
    std::map<std::string, std::vector<std::string>> stacks;
    for (const DWORD id : ids) {
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                       THREAD_QUERY_LIMITED_INFORMATION,
                                   FALSE, id);
        if (!thread) {
            continue;
        }
        std::string name = Describe(thread);
        DWORD64 frames[kMaxFrames] = {};
        const int count = Walk(thread, frames);
        CloseHandle(thread);
        std::string stack;
        for (int i = 0; i < count; ++i) {
            stack += i ? " < " : "";
            stack += Module(frames[i]);
        }
        name = (name.empty() ? "" : name + " ") + "#" + std::to_string(id);
        stacks[stack].push_back(name);
    }
    for (const auto& [stack, names] : stacks) {
        std::string list;
        for (size_t i = 0; i < names.size(); ++i) {
            list += (i ? ", " : "") + names[i];
        }
        std::printf("Freeze: %s: %s\n", list.c_str(), stack.empty() ? "(no stack)" : stack.c_str());
    }
    std::printf("== Freeze: end of report\n");
    std::fflush(stdout);
}

void Watch() {
    const char* env = std::getenv("BB_FREEZE_REPORT_S");
    const long long limit_ms = (env ? std::atoll(env) : 10) * 1000;
    if (limit_ms <= 0) {
        return;
    }
    std::thread([limit_ms] {
        SetThreadDescription(GetCurrentThread(), L"bb:FreezeWatch");
        bool reported = false;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const long long frozen = NowMs() - last_flip_ms.load(std::memory_order_relaxed);
            if (frozen < limit_ms) {
                reported = false;
            } else if (!reported) {
                Report(frozen);
                reported = true;
            }
        }
    }).detach();
}
}

void NoteFlip() {
    last_flip_ms.store(NowMs(), std::memory_order_relaxed);
    std::call_once(started, Watch);
}

void SetStateReporter(std::function<void()> reporter) {
    {
        std::scoped_lock lock{reporter_mutex};
        state_reporter = std::move(reporter);
    }
    static std::once_flag first_frame_watch;
    std::call_once(first_frame_watch, [] {
        const char* env = std::getenv("BB_FIRST_FRAME_REPORT_S");
        const long long limit_s = env ? std::atoll(env) : 45;
        if (limit_s <= 0) {
            return;
        }
        std::thread([limit_s] {
            SetThreadDescription(GetCurrentThread(), L"bb:FirstFrameWatch");
            std::this_thread::sleep_for(std::chrono::seconds(limit_s));
            const auto drawn = BbStats::draws.load(std::memory_order_relaxed);
            if (last_flip_ms.load(std::memory_order_relaxed) == 0 || drawn < 200) {
                std::printf("== Startup: %lld s after the start the game has drawn %llu things "
                            "(%s); where its threads are follows\n",
                            limit_s, static_cast<unsigned long long>(drawn),
                            last_flip_ms.load() ? "it flips frames" : "no frame yet");
                Report(limit_s * 1000);
            }
        }).detach();
    });
}
}

#else
namespace BbFreeze {
void NoteFlip() {}
void SetStateReporter(std::function<void()>) {}
}
#endif
