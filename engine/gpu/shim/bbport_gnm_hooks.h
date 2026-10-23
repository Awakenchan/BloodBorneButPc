// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace BbGnmHooks {
void PatchImage(unsigned char* image, std::uint64_t size);

void CheckSubmission(const std::uint32_t* commands, std::uint64_t dwords);

struct DriverWrite {
    std::uint32_t* commands;
    std::uint64_t dwords;
    ~DriverWrite();
};
}
