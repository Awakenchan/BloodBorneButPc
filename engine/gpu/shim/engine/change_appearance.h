// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace BbEngine::ChangeAppearance {
bool Watch(std::uint8_t* image);
void EnableEditor();
void Tick();
bool PreviewShown();
}
