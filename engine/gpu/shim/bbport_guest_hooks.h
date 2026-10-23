// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace BbGuestHooks {
using RangeCallback = void (*)(std::uint64_t address, std::uint64_t size);

void Install(RangeCallback on_gpu_range_allocated);
}
