// SPDX-License-Identifier: GPL-2.0-or-later
#include "trophy_sync.h"
#include "engine_state.h"
#include <bitset>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace BbEngine::TrophySync {
namespace {
struct Award {
    int trophy;
    std::uint32_t event;
    std::uint32_t ending = 0;
};
constexpr Award kAwards[] = {
    {1, 12100180, 6600},  {2, 12100000, 6601},  {3, 12100002, 6602},
    {12, 12110990},
    {23, 12201800},
    {22, 12301800}, {24, 12301700},
    {15, 12401800},
    {21, 12411700}, {14, 12411800},
    {28, 12421800}, {27, 12421700}, {11, 12420990},
    {10, 12500000}, {26, 12501800},
    {20, 12601800}, {19, 12601850},
    {16, 12701800},
    {18, 12801800},
    {17, 13201800}, {13, 13200960},
    {25, 13301800},
    {36, 13401800}, {39, 13401850},
    {37, 13501800}, {38, 13501850},
    {35, 13601800},
};
constexpr int kCount = int(sizeof(kAwards) / sizeof(kAwards[0]));

std::bitset<kCount> g_reported;
std::chrono::steady_clock::time_point g_next{};
std::uint64_t g_read_for = 0;

bool Set(std::uint32_t flag) {
    bool on = false;
    return flag && event_flag_get(flag, &on) && on;
}
}

void Tick() {
    if (g_reported.all()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_next) return;
    g_next = now + std::chrono::seconds(3);
    const std::uint64_t record = player_record();
    if (!record || !world_player_block(nullptr)) {
        g_read_for = 0;
        return;
    }
    if (record == g_read_for) return;
    g_read_for = record;
    for (int i = 0; i < kCount; ++i) {
        if (g_reported.test(i) || !(Set(kAwards[i].event) || Set(kAwards[i].ending))) continue;
        g_reported.set(i);
        std::printf("Runtime: trophy %d earned in this save\n", kAwards[i].trophy);
    }
}
}
