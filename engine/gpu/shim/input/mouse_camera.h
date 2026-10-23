// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace MouseCamera {

bool Install(std::uint8_t* image, std::size_t size);
bool Installed();

void AddCounts(float dx, float dy);

void Reset();

void Publish(int sensitivity, bool invert_x, bool invert_y, bool hold_auto_rotation);

bool Flick(float& x, float& y);
}
