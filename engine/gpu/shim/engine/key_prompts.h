// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace KeyPrompts {

constexpr std::size_t kIconBytes = 0x400, kHudBytes = 0x80000;

int Identify(const void* data, std::size_t size, std::uint32_t width, std::uint32_t height);

bool Draw(int texture, std::uint8_t* data, std::size_t size);

bool Changed();

}
