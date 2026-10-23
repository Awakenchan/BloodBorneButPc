// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/rebirth_cost.h"
#include <cmath>
#include <cstdio>

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
}

int main() {
    using namespace BbEngine::Rebirth;
    const LevelCurve curve{0.1f, 1.0f, 0.02f, 92.0f};
    Check(LevelCost(curve, 4) == 724 && LevelCost(curve, 10) == 829 && LevelCost(curve, 11) == 847,
          "the level-up screen's prices: 724 from level 4, 829 from 10, 847 from 11");
    Check(LevelCost(curve, 50) == 15103, "past the curve's knee (x = 131)");

    const Hunter waste{4, 0, {10, 9, 10, 9, 7, 9}};
    Hunter now = waste;
    Check(Refund(now, waste, curve) == 0, "nothing gained, nothing given back");
    now.level = 6, now.stats[0] = 11, now.stats[2] = 11;
    Check(Refund(now, waste, curve) == 724 + 741, "two points: levels 4 and 5");
    now.level = 40;
    Check(Refund(now, waste, curve) == 724 + 741, "priced by points, not the level the save says");
    now.stats[4] = 6;
    Check(Refund(now, waste, curve) == 724 + 741, "a point below the origin is not a credit");

    now = waste;
    for (auto& s : now.stats) s = 99;
    now.level = kLevelLimit;
    const auto all = Refund(now, waste, curve);
    Check(all && *all > 0 && *all <= kEchoLimit, "every attribute at 99 fits what a hunter can hold");
    now.echoes = kEchoLimit - *all + 1;
    Check(!Refund(now, waste, curve), "a refund past the echo limit is refused");

    now = waste;
    now.stats[1] = 100;
    Check(!Refund(now, waste, curve), "an attribute past 99 is refused");
    now = waste;
    now.echoes = -1;
    Check(!Refund(now, waste, curve), "negative echoes are refused");
    Check(!Refund(waste, Hunter{}, curve), "an origin with no attributes is refused");
    const LevelCurve broken{std::nanf(""), 1, 0.02f, 92};
    now = waste;
    now.stats[0] = 11;
    Check(!Refund(now, waste, broken), "a curve that is not a number is refused");
    if (failures) return 1;
    std::puts("PASS: rebirth refund: the game's prices, points over level, limits and implausible hunters");
    return 0;
}
