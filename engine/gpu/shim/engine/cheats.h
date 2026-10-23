// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace BbEngine::Cheats {
enum Which : int {
    Vials = 0, Bullets, Stamina, Stealth, GunRange, KeepEchoes, EchoMultiplier, NoDurability, Consumables,
    Echoes,
    WhichCount
};
bool Install(std::uint8_t* image, std::size_t size);
void Tick();
const char* Unavailable(Which which);
void AddEchoes(int amount);
void AddInsight(int amount);
}
