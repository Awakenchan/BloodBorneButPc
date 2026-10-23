// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
#include <cstdint>
#include <optional>

namespace BbEngine::Rebirth {

constexpr std::int64_t kEchoLimit = 999999999;
constexpr std::int64_t kLevelLimit = 544;

struct Hunter {
    std::int64_t level = 0, echoes = 0;
    std::int64_t stats[6]{};
};

struct LevelCurve {
    float base = 0, offset = 0, steep = 0, knee = 0;
};

inline std::optional<std::int64_t> LevelCost(const LevelCurve& c, std::int64_t level) {
    const float x = static_cast<float>(level + 81);
    const float over = std::fmax(0.0f, x - c.knee) * c.steep;
    const float rate = over + c.base;
    const float square = x * x;
    const float price = square * rate;
    const float echoes = price + c.offset + 0.5f;
    if (!std::isfinite(echoes) || echoes < 0 || echoes > static_cast<float>(kEchoLimit)) return std::nullopt;
    return static_cast<std::int64_t>(echoes);
}

inline bool Plausible(const Hunter& h) {
    if (h.level < 1 || h.level > kLevelLimit || h.echoes < 0 || h.echoes > kEchoLimit) return false;
    for (const std::int64_t s : h.stats)
        if (s < 1 || s > 99) return false;
    return true;
}

inline std::optional<std::int64_t> Refund(const Hunter& now, const Hunter& origin, const LevelCurve& curve) {
    if (!Plausible(now) || !Plausible(origin)) return std::nullopt;
    std::int64_t points = 0;
    for (int i = 0; i < 6; ++i) points += now.stats[i] > origin.stats[i] ? now.stats[i] - origin.stats[i] : 0;
    if (origin.level + points > kLevelLimit) return std::nullopt;
    std::int64_t echoes = 0;
    for (std::int64_t level = origin.level; level < origin.level + points; ++level) {
        const auto cost = LevelCost(curve, level);
        if (!cost || *cost > kEchoLimit - echoes) return std::nullopt;
        echoes += *cost;
    }
    if (echoes > kEchoLimit - now.echoes) return std::nullopt;
    return echoes;
}

}
