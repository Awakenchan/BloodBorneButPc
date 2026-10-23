// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: GPU work left out or merged, with kill switches, and its BB_FRAME_STATS report.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace Vulkan::GpuSavings {

/// An environment switch on by default: BB_x=0 turns it off.
inline bool EnvOn(const char* name) {
    const char* env = std::getenv(name);
    return !(env && env[0] == '0');
}

/// BB_IMAGE_TILE_MEMO=0: a texel buffer aliasing an image is filled from the image (download +
/// tiling) at every binding, as before, also when neither changed since the last fill.
inline bool TileMemoOn() {
    static const bool on = EnvOn("BB_IMAGE_TILE_MEMO");
    return on;
}
/// BB_HLE_COPY_MERGE=1: the copy shader's batches merged into one dispatch per guest dispatch.
/// Off by default: rainbow noise in some ground textures was seen with it on (beta 8 test).
inline bool HleMergeOn() {
    static const bool on = [] {
        const char* env = std::getenv("BB_HLE_COPY_MERGE");
        return env && env[0] == '1';
    }();
    return on;
}

struct Counters {
    // Texel buffer fills from images (BufferCache::SynchronizeMemoryFromImage).
    std::atomic<std::uint64_t> tile_fills{0}, tile_skips{0}, tile_bytes_skipped{0};
    // Why a fill was not skipped although a memo for the range existed.
    std::atomic<std::uint64_t> tile_miss_image{0}, tile_miss_buffer{0}, tile_miss_proxy{0},
        tile_miss_other{0};
    // HLE copy shader: batches, and dispatches/barrier checks recorded for them.
    std::atomic<std::uint64_t> hle_batches{0}, hle_dispatches{0}, hle_barriers{0};
    // Lookahead thread: sleeps instead of yields while stage B is stuck (BB_LOOKAHEAD_SLEEP).
    std::atomic<std::uint64_t> lookahead_sleeps{0};
    // Scene proxy resolves by the native access that needed them: transfer (copies, downloads,
    // texel buffer fills), sampled read, storage write, native attachment, other.
    std::array<std::atomic<std::uint64_t>, 5> scene_resolves{};
};
inline Counters& Get() {
    static Counters counters;
    return counters;
}

/// BB_FRAME_STATS: one line every 5 s (called once a submission).
inline void Report() {
    static auto window = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - window < std::chrono::seconds(5)) {
        return;
    }
    const double s = std::chrono::duration<double>(now - window).count();
    window = now;
    auto& c = Get();
    const auto take = [](std::atomic<std::uint64_t>& v) { return double(v.exchange(0)); };
    const double fills = take(c.tile_fills), skips = take(c.tile_skips);
    const double skipped_mb = take(c.tile_bytes_skipped) / (1024.0 * 1024.0);
    const double mi = take(c.tile_miss_image), mb = take(c.tile_miss_buffer),
                 mp = take(c.tile_miss_proxy), mo = take(c.tile_miss_other);
    const double batches = take(c.hle_batches), dispatches = take(c.hle_dispatches),
                 barriers = take(c.hle_barriers);
    const double sleeps = take(c.lookahead_sleeps);
    double resolves[5];
    for (int i = 0; i < 5; ++i) {
        resolves[i] = take(c.scene_resolves[i]);
    }
    std::printf("GPU savings per second: image->texel buffer fills %.1f, skipped unchanged %.1f "
                "(%.1f MB; refilled after image write %.1f, buffer write %.1f, proxy %.1f, other "
                "%.1f) [BB_IMAGE_TILE_MEMO]; HLE copy %.0f batches in %.0f dispatches, %.0f "
                "barrier flushes [BB_HLE_COPY_MERGE]; lookahead sleeps %.0f "
                "[BB_LOOKAHEAD_SLEEP]\n",
                fills / s, skips / s, skipped_mb / s, mi / s, mb / s, mp / s, mo / s, batches / s,
                dispatches / s, barriers / s, sleeps / s);
    std::printf("  scene proxy resolves per second by native access: transfer %.1f, sampled %.1f, "
                "storage %.1f, attachment %.1f, other %.1f\n",
                resolves[0] / s, resolves[1] / s, resolves[2] / s, resolves[3] / s, resolves[4] / s);
}

} // namespace Vulkan::GpuSavings
