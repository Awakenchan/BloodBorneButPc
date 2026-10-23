// SPDX-License-Identifier: GPL-3.0-or-later
#include "menu_memory.h"
#include <cstdio>
#include <cstring>

namespace BbEngine::MenuMemory {
namespace {
constexpr std::size_t kScaleformHeap = 0x1f5a465, kScaleformPages = 0x1f5a479;
constexpr std::size_t kMenuBudget = 0x4736d40 + 2 * 0x80 + 9 * 8;
constexpr std::uint8_t kMovEdx = 0xba;
constexpr std::uint32_t kGameHeap = 18u << 20, kGamePages = 34u << 20;
constexpr std::uint64_t kGameBudget = 82ull << 20;
constexpr std::uint32_t kMoreHeap = 32u << 20, kMorePages = 64u << 20;
constexpr std::uint64_t kMoreBudget = 96ull << 20;

template <typename T> T At(const std::uint8_t* image, std::size_t at) {
    T v;
    std::memcpy(&v, image + at, sizeof v);
    return v;
}
}

bool Compatible(const std::uint8_t* image, std::size_t size) {
    if (size < kMenuBudget + 8 || size < kScaleformPages + 5) return false;
    return image[kScaleformHeap] == kMovEdx && image[kScaleformPages] == kMovEdx &&
           At<std::uint32_t>(image, kScaleformHeap + 1) == kGameHeap &&
           At<std::uint32_t>(image, kScaleformPages + 1) == kGamePages && At<std::uint64_t>(image, kMenuBudget) == kGameBudget;
}

bool Install(std::uint8_t* image, std::size_t size) {
    if (!Compatible(image, size)) return false;
    const std::uint32_t heap = kGameHeap + kMoreHeap, pages = kGamePages + kMorePages;
    const std::uint64_t budget = kGameBudget + kMoreBudget;
    std::memcpy(image + kScaleformHeap + 1, &heap, sizeof heap);
    std::memcpy(image + kScaleformPages + 1, &pages, sizeof pages);
    std::memcpy(image + kMenuBudget, &budget, sizeof budget);
    std::printf("Engine: Scaleform menu heaps %u/%u MiB, MENU %llu MiB (native option pages)\n", heap >> 20,
                pages >> 20, static_cast<unsigned long long>(budget >> 20));
    return true;
}
}
