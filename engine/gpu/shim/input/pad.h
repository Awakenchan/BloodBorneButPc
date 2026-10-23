// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace PadBits {
constexpr std::uint32_t kL3 = 0x2, kR3 = 0x4, kOptions = 0x8;
constexpr std::uint32_t kUp = 0x10, kRight = 0x20, kDown = 0x40, kLeft = 0x80;
constexpr std::uint32_t kDpad = kUp | kRight | kDown | kLeft;
constexpr std::uint32_t kL2 = 0x100, kR2 = 0x200, kL1 = 0x400, kR1 = 0x800;
constexpr std::uint32_t kTriangle = 0x1000, kCircle = 0x2000, kCross = 0x4000, kSquare = 0x8000;
constexpr std::uint32_t kTouchPad = 0x100000;
}

constexpr std::uint8_t kStickRest = 127, kStickLow = 0, kStickHigh = 254;

constexpr int kTouchWidth = 1920, kTouchHeight = 943;

struct PadState {
    std::uint32_t buttons = 0;
    std::uint8_t lx = kStickRest, ly = kStickRest, rx = kStickRest, ry = kStickRest;
    std::uint8_t l2 = 0, r2 = 0;
    struct Finger {
        std::uint16_t x = 0, y = 0;
        std::uint8_t id = 0;
    } touch[2];
    std::uint8_t touch_count = 0;
};
