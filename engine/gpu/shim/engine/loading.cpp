// SPDX-License-Identifier: GPL-3.0-or-later
// Load monitor: how long each loading screen is up and how the game paces its frames meanwhile.
// The flag and helper offsets follow bbhost (src/engine/loading.cpp); the flag is checked against
// this executable: 17 instructions in the loading code (0x1763db0..0x193fb46) set it to 1 when a
// load starts and back to 0, and the frame pacer at 0x2034770 keeps its vblank count at +0x10 and
// its frame-time target at +0x18 (1/30 = 0x3d088889, 1/60 = 0x3c888889 before the FPS patches).
#include "loading.h"
#include "engine_hooks.h"
#include "bbport_platform.h"
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <cstdio>

namespace BbEngine::Loading {
namespace {
constexpr std::size_t kNowLoadingRequested = 0x556286b; // u8
constexpr std::size_t kNowLoadingHelper = 0x553e8b8;    // CSNowLoadingHelper*, in game at +0x50/+0x51
constexpr std::size_t kSprjRemo = 0x5540058;            // cutscene player: manager at +8
std::uint8_t* g_base = nullptr;
std::atomic<bool> g_up{false};
using Clock = std::chrono::steady_clock;
Clock::time_point g_start, g_began;
std::uint64_t g_frames = 0;
bool g_seen_world = false;

// Loading pace (BB_FAST_LOADING=1, the launcher's "Fast loading"): while a loading screen is up the
// four instructions that set the pacer's target (mov dword [r12+0x18], frame time; the FPS patches'
// frame sites) give 1/30 s, and they go back when the load ends. Measured: loads run on the main
// thread's per-frame work beside the decompression thread (Core.Res.PostProcessor); uncapped (3.3 ms
// frames) the first load took 49.1 s instead of 19.6 s at 120 FPS, the main thread at 65% in game
// code and the decompression thread down from ~40% to ~10%. Fewer frames leave the loading work more
// room. Not at 30 FPS (nothing to change) and not when a cutscene follows (timed for the frame rate).
constexpr std::size_t kTargetSites[] = {0x2034840, 0x203485a, 0x203487e, 0x20348b3};
constexpr std::uint8_t kTargetStore[] = {0x41, 0xc7, 0x44, 0x24, 0x18}; // mov dword [r12+0x18], imm32
constexpr float kFastTarget = 1.0f / 30.0f;
bool g_fast_allowed = false, g_fast_on = false, g_fast_this_load = false;
float g_saved[4]{};
// The pacer's target when the load began: the restored instructions only set the live target when
// the game runs them again, which it may not do for minutes after a load (two loads back to back,
// a lantern travel: FPS stuck at 29 until the next one, 0.9.37 beta 7).
float g_target_before = 0;

void SetFast(bool on) {
    if (on == g_fast_on || !g_fast_allowed) return;
    for (int i = 0; i < 4; ++i) {
        std::uint8_t* imm = g_base + kTargetSites[i] + sizeof(kTargetStore);
        if (on) std::memcpy(&g_saved[i], imm, sizeof(float));
        const float value = on ? kFastTarget : g_saved[i];
        WriteCode(imm, &value, sizeof value);
    }
    g_fast_on = on;
    g_fast_this_load |= on;
}

template <typename T> bool Read(std::uint64_t address, T& value) {
    return address && BbPlatform::ReadProcessMemory(reinterpret_cast<void*>(address), &value, sizeof value);
}

struct Pace {
    std::uint32_t mode = 0, vblanks = 0;
    float target = 0;
};
Pace PaceOf(std::uint64_t flipper) {
    Pace p;
    Read(flipper + 0xc, p.mode);
    Read(flipper + 0x10, p.vblanks);
    Read(flipper + 0x18, p.target);
    return p;
}
} // namespace

void Install(std::uint8_t* image) {
    g_base = image;
    g_start = Clock::now();
    const char* fast = std::getenv("BB_FAST_LOADING");
    const char* fps = std::getenv("BB_FPS");
    if (!fast || fast[0] != '1') return;
    if (fps && !std::strcmp(fps, "30")) {
        std::printf("Loading: fast loading not needed at 30 FPS\n");
        return;
    }
    for (std::size_t at : kTargetSites)
        if (std::memcmp(image + at, kTargetStore, sizeof kTargetStore)) {
            std::printf("Loading: fast loading off: the frame pacer's bytes differ from verified 1.09\n");
            return;
        }
    g_fast_allowed = true;
    std::printf("Loading: fast loading on (loading screens paced at 30 FPS)\n");
}

bool ScreenUp() { return g_up.load(std::memory_order_relaxed); }

void Tick(std::uint64_t flipper) {
    if (!g_base) return;
    const auto base = reinterpret_cast<std::uint64_t>(g_base);
    std::uint8_t requested = 0, shown_a = 0, shown_b = 0;
    std::uint64_t helper = 0;
    Read(base + kNowLoadingRequested, requested);
    const bool in_game = Read(base + kNowLoadingHelper, helper) && helper &&
                         ((Read(helper + 0x50, shown_a) && shown_a) || (Read(helper + 0x51, shown_b) && shown_b));
    const bool loading = requested && !in_game;
    const auto now = Clock::now();
    ++g_frames;
    const bool was = g_up.exchange(loading, std::memory_order_relaxed);
    if (g_fast_allowed) {
        bool cutscene = false;
        if (loading) {
            std::uint64_t remo = 0, man = 0, queued = 0;
            std::uint32_t running = 0;
            cutscene = Read(base + kSprjRemo, remo) && remo && Read(remo + 8, man) && man &&
                       ((Read(man + 0x168, running) && (running & 1)) || (Read(man + 0xf8, queued) && queued));
        }
        SetFast(loading && !cutscene);
    }
    if (loading && !was) {
        g_began = now;
        g_frames = 0;
        const Pace p = PaceOf(flipper);
        // A load that begins while the last one's pace is still set keeps the earlier target.
        if (p.target != kFastTarget) g_target_before = p.target;
        std::printf("Loading: begins; pacer mode %u, %u vblank(s) a frame, target %.1f ms\n", p.mode, p.vblanks,
                    p.target * 1000.0f);
    } else if (!loading && was) {
        const double seconds = std::chrono::duration<double>(now - g_began).count();
        std::uint64_t remo = 0, man = 0, queued = 0;
        std::uint32_t running = 0;
        const bool cutscene = Read(base + kSprjRemo, remo) && remo && Read(remo + 8, man) && man &&
                              ((Read(man + 0x168, running) && (running & 1)) || (Read(man + 0xf8, queued) && queued));
        // The live target back to what it was before the load, if it is still the loading pace this
        // load set (a cutscene sets its own pace: left alone). The game's own store at +0x18.
        float live = 0;
        if (g_fast_this_load && !cutscene && g_target_before > 0 && Read(flipper + 0x18, live) &&
            live == kFastTarget) {
            *reinterpret_cast<volatile float*>(flipper + 0x18) = g_target_before;
            std::printf("Loading: the frame pace back to %.1f ms (the loading pace stayed set)\n",
                        g_target_before * 1000.0f);
        }
        g_fast_this_load = false;
        const Pace p = PaceOf(flipper);
        std::printf("Loading: %.2f s, %llu frames (%.1f ms each)%s; pacer mode %u, %u vblank(s), target %.1f ms\n",
                    seconds, static_cast<unsigned long long>(g_frames),
                    g_frames ? seconds * 1000.0 / static_cast<double>(g_frames) : 0.0,
                    cutscene ? ", a cutscene follows" : "", p.mode, p.vblanks, p.target * 1000.0f);
    }
    if (in_game && !g_seen_world) {
        g_seen_world = true;
        std::printf("Loading: the first in-game frame, %.1f s after the game started\n",
                    std::chrono::duration<double>(now - g_start).count());
    }
}
} // namespace BbEngine::Loading
