// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace BbEngine::OptionKeys {
void __attribute__((sysv_abi)) Build(void* dialog, void* params);
void Update(std::uint8_t* list, bool focused);
bool ClickDecides(const std::uint8_t* list, int row);
}
