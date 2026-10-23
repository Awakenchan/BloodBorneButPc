// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

std::uint32_t menu_confirm_button();
std::uint32_t menu_back_button();

bool menu_pointer_in_menu();

namespace MenuPointer {

bool Install(std::uint8_t* image, std::size_t size);

bool FrontEnd();

bool Navigating();

void Click();

std::uint32_t TakePress();

void HoldRow(bool hold);
}
