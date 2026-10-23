// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Keycap {

constexpr int kSize = 32;
constexpr std::size_t kBytes = 0x400;

enum PadButton : int {
    PadA, PadB, PadX, PadY, PadLB, PadRB, PadLT, PadRT, PadLS, PadRS, PadView, PadMenu, PadShare,
    PadUp, PadDown, PadLeft, PadRight, PadP1, PadP2, PadP3, PadP4, PadTouchpad, PadGuide,
};

struct Spec {
    enum Kind : std::uint8_t {
        Key,
        Keys,
        Mouse,
        Pad,
    };
    Kind kind = Key;
    std::vector<std::string> labels;
    int button = 0;

    std::string Id() const;
};

void Draw(const Spec& spec, std::uint8_t* rgba, int size = kSize);
void Encode(const std::uint8_t rgba[kSize * kSize * 4], std::uint8_t out[kBytes]);
void EncodeBlocks(const std::uint8_t* rgba, int width, int height, std::uint8_t* out);

}
