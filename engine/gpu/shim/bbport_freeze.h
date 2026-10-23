// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>

namespace BbFreeze {
void NoteFlip();
void SetStateReporter(std::function<void()> reporter);
}
