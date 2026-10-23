// SPDX-License-Identifier: GPL-3.0-or-later
// The game patches (was scripts/patches.py): the frame-rate preset and its corrections, the effect
// switches, the resolution, the built-in and third-party XML patches the player switched on,
// compiled into out/patches.bin (BBPATCH2) for the loader.
#pragma once
#include "common.h"

namespace start {

struct Size {
    int w = 0, h = 0;
    bool operator==(const Size&) const = default;
};
std::optional<Size> ParseSize(std::string_view text);

/// (render, output) when lantern.ini selects an output other than 1080p (patches.py scaled_sizes).
std::optional<std::pair<Size, Size>> ScaledSizes(const std::map<std::string, std::string>& settings);

struct PatchRequest {
    fs::path out, xml, settings, game;
    fs::path patches_dir, patches_config;
    std::string fps, extra, render_res, output_res;
};
struct PatchResult {
    bool heap_patch = false;
    Size ui{1920, 1080};
};
PatchResult CompilePatches(const PatchRequest& request);

} // namespace start
