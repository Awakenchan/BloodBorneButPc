// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace LanternPacing {
using Clock = std::chrono::steady_clock;

bool VblankPacing();

void WatchVblank(const wchar_t* device);
struct VblankTime {
    Clock::time_point last;
    std::chrono::nanoseconds period;
};
std::optional<VblankTime> Vblank();

bool Enabled();

bool Sleep(std::chrono::nanoseconds duration, bool alertable = false);

void WaitUntil(Clock::time_point deadline, bool exact,
               std::chrono::nanoseconds max_spin = std::chrono::microseconds(1500));

class Cadence {
public:
    explicit Cadence(std::chrono::nanoseconds period, Clock::time_point start = Clock::now())
        : period{period}, slot{start} {}

    void SetPeriod(std::chrono::nanoseconds new_period) {
        period = new_period;
    }
    [[nodiscard]] std::chrono::nanoseconds Period() const {
        return period;
    }
    [[nodiscard]] Clock::time_point Slot() const {
        return slot;
    }
    [[nodiscard]] bool Ready(Clock::time_point now) const {
        return now + period / 16 >= slot;
    }
    void Taken(Clock::time_point now) {
        slot = now - slot > period / 4 ? now + period : slot + period;
    }
    void Tick(Clock::time_point now) {
        slot = now - slot > period ? now + period : slot + period;
    }
    void Align(Clock::time_point vblank, std::chrono::nanoseconds refresh,
               std::chrono::nanoseconds offset) {
        const std::int64_t r = refresh.count();
        const auto target = vblank + offset;
        const std::int64_t ahead = (slot - target).count();
        const std::int64_t k = ahead >= 0 ? (ahead + r / 2) / r : -((-ahead + r / 2) / r);
        slot = target + refresh * k;
    }

private:
    std::chrono::nanoseconds period;
    Clock::time_point slot;
};
}
