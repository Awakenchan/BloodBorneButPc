// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace rebirth {
constexpr std::uint32_t kReqReset = 72500390, kReqUndo = 72500391, kFail = 72500392, kMenu = 72500393;
constexpr std::int32_t kStone = 4330;
constexpr std::int32_t kTextOption = 14000332, kTextNoStone = 14000333, kTextFail = 14000334,
                       kTextAccept = 14000335, kTextUndo = 14000336;
}

bool rebirth_script(std::vector<std::uint8_t>& esd, std::string* why);
