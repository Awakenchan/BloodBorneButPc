// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace BbInputConfig {
using Items = std::vector<std::pair<std::string, std::string>>;
inline void (*append_bindings)(Items& lines) = nullptr;

inline void Append(Items& lines) {
    if (append_bindings) append_bindings(lines);
}
}
