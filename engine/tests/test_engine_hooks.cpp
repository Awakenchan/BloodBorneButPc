// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/engine_hooks.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

struct Seen {
    unsigned calls = 0;
    std::uint64_t args[6]{};
    double xmm[2]{};
    bool skip = false;
    std::uint64_t value = 0;
} seen;

std::int64_t __attribute__((sysv_abi)) Watch(std::uint64_t, const std::uint64_t* saved) {
    ++seen.calls;
    for (unsigned i = 0; i < 6; ++i) seen.args[i] = saved[5 - i];
    std::memcpy(&seen.xmm[0], BbEngine::SavedXmm(saved, 0), 8);
    std::memcpy(&seen.xmm[1], BbEngine::SavedXmm(saved, 1), 8);
    __asm__ volatile("xorps %%xmm0, %%xmm0\n\txorps %%xmm1, %%xmm1" ::: "xmm0", "xmm1");
    if (!seen.skip) return 0;
    const_cast<std::uint64_t*>(saved)[-1] = seen.value;
    return 1;
}

std::uint8_t* Executable(std::size_t n) {
#ifdef _WIN32
    return static_cast<std::uint8_t*>(VirtualAlloc(nullptr, n, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
#else
    void* p = mmap(nullptr, n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<std::uint8_t*>(p);
#endif
}

__attribute__((noinline)) std::uint64_t CallWithRax(const void* fn, std::uint64_t rax) {
    std::uint64_t out = rax;
    __asm__ volatile("sub $128, %%rsp\n\t"
                     "call *%[fn]\n\t"
                     "add $128, %%rsp"
                     : "+a"(out)
                     : [fn] "r"(fn)
                     : "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "cc", "xmm0", "xmm1",
                       "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11",
                       "xmm12", "xmm13", "xmm14", "xmm15");
    return out;
}

template <std::size_t N> void Place(std::uint8_t* at, const std::uint8_t (&code)[N]) {
    std::memcpy(at, code, N);
}
}

int main() {
    std::uint8_t* image = Executable(8192);
    if (!image) { std::puts("FAIL: no executable memory"); return 1; }
    std::array<std::uint8_t, 16> entry;
    entry.fill(0x90);
    const std::uint8_t sum[] = {0x48, 0x89, 0xf8, 0x48, 0x01, 0xf0, 0x48, 0x01, 0xd0, 0x48, 0x01,
                                0xc8, 0x4c, 0x01, 0xc0, 0x4c, 0x01, 0xc8, 0xc3};
    const auto fn = reinterpret_cast<std::uint64_t>(image);
    std::memcpy(image, entry.data(), entry.size());
    Place(image + 16, sum);

    BbEngine::Hook hook;
    auto other = entry;
    other[3] = 0xcc;
    Check(!BbEngine::Prepare(hook, image, 8192, 0, other, Watch), "a site with other bytes is refused");
    Check(!BbEngine::Prepare(hook, image, 10, 0, entry, Watch), "a site past the image's end is refused");
    Check(!BbEngine::Prepare(hook, image, 8192, 0, std::span(entry).first(13), Watch),
          "fewer bytes than the jump are refused");
    Check(!hook.stub && !std::memcmp(image, entry.data(), entry.size()), "a refused hook changes nothing");

    Check(BbEngine::Call(fn, 1, 2, 3, 4, 5, 6) == 21, "the bridge passes six arguments");
    Check(BbEngine::Call(fn, 100) == 100, "registers a call does not fill are zero");
    Check(BbEngine::Prepare(hook, image, 8192, 0, entry, Watch), "a matching site is prepared");
    Check(!std::memcmp(image, entry.data(), entry.size()), "Prepare leaves the image alone");
    BbEngine::Commit(std::span(&hook, 1));
    Check(image[0] == 0xff && image[1] == 0x25 && image[14] == 0xcc && image[15] == 0xcc,
          "Commit writes the jump and fills the rest with int3");
    Check(BbEngine::Call(fn, 1, 2, 3, 4, 5, 6) == 21, "the hooked function runs with its arguments");
    Check(seen.calls == 1, "the callback runs once per call");
    Check(seen.args[0] == 1 && seen.args[2] == 3 && seen.args[5] == 6, "saved[] holds r9 down to rdi");

    seen.skip = true;
    seen.value = 0x1234;
    Check(BbEngine::Call(fn, 1, 2, 3, 4, 5, 6) == 0x1234, "a skipped call returns saved[-1]");
    seen.skip = false;

    const std::uint8_t just_return[] = {0xc3};
    Place(image + 16, just_return);
    Check(CallWithRax(image, 0x5a5a) == 0x5a5a, "rax survives the stub");
    BbEngine::Discard(hook);
    Check(!hook.stub, "Discard clears the hook");

    std::memcpy(image, entry.data(), entry.size());
    const std::uint8_t add_doubles[] = {0xf2, 0x0f, 0x58, 0xc1, 0xc3};
    Place(image + 16, add_doubles);
    Check(BbEngine::Prepare(hook, image, 8192, 0, entry, Watch), "a second hook on the same site");
    BbEngine::Commit(std::span(&hook, 1));
    using Doubles = double (__attribute__((sysv_abi)) *)(double, double);
    Check(reinterpret_cast<Doubles>(image)(1.25, 3.5) == 4.75, "xmm0-1 reach the function");
    Check(seen.xmm[0] == 1.25 && seen.xmm[1] == 3.5, "SavedXmm reads xmm0-1");
    BbEngine::Discard(hook);

    const std::uint8_t seventh[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x8b, 0x45, 0x10, 0x4c, 0x01, 0xc8, 0x5d, 0xc3};
    Place(image + 0x100, seventh);
    Check(BbEngine::Call(fn + 0x100, 1, 2, 3, 4, 5, 6, 70) == 76, "the seventh argument is on the stack");

    seen = {};
    Place(image + 0x300, sum);
    std::uint8_t* tail = BbEngine::TailStub(Watch, fn + 0x300);
    Check(tail != nullptr, "a tail stub is made");
    if (tail) {
        std::uint8_t* p = image + 0x210;
        *p++ = 0x49;
        *p++ = 0xbb;
        const auto to = reinterpret_cast<std::uint64_t>(tail);
        std::memcpy(p, &to, 8);
        p += 8;
        *p++ = 0x41;
        *p++ = 0xff;
        *p = 0xe3;
        image[0x200] = 0xe9;
        const std::int32_t rel = 0x210 - (0x200 + 5);
        std::memcpy(image + 0x201, &rel, 4);
        Check(BbEngine::Call(fn + 0x200, 1, 2, 3, 4, 5, 6) == 21, "the tail call's target gets the arguments");
        Check(seen.calls == 1 && seen.args[4] == 5, "the tail stub calls the host first");
        BbEngine::FreeStub(tail);
    }

    std::vector<std::uint8_t*> many;
    for (int i = 0; i < 40; ++i) many.push_back(BbEngine::TailStub(Watch, fn + 0x300));
    bool all = true;
    for (auto* s : many) all = all && s;
    Check(all, "forty stubs are made");
    for (auto* s : many) BbEngine::FreeStub(s);
    std::uint8_t* again = BbEngine::TailStub(Watch, fn + 0x300);
    Check(again != nullptr, "a freed page is used again");
    BbEngine::FreeStub(again);

    const std::uint8_t five[] = {0xb8, 5, 0, 0, 0, 0xc3};
    Place(image + 0x400, five);
#ifdef _WIN32
    DWORD before = 0;
    VirtualProtect(image, 8192, PAGE_EXECUTE_READ, &before);
#else
    mprotect(image, 8192, PROT_READ | PROT_EXEC);
#endif
    const std::uint8_t seven = 7;
    Check(BbEngine::WriteCode(image + 0x401, &seven, 1), "WriteCode writes into read-execute code");
    Check(BbEngine::Call(fn + 0x400) == 7, "the written code runs");

    if (failures) return 1;
    std::puts("PASS: hook guards, staged commit, saved registers and XMM, skip with saved[-1], rax kept, "
              "seven-argument bridge, tail stubs, stub pages reused, WriteCode");
    return 0;
}
