// SPDX-License-Identifier: GPL-2.0-or-later
// Lantern Engine: frame pacing. Waits that land on time (an OS timer sleep for most of the wait,
// then a spin as long as the timer's recent wake-ups needed) and an even cadence of slots for
// vblank ticks and flips. On by default on Windows; BB_PRECISE_PACING=0 turns it off (=1 on Linux).
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace LanternPacing {
using Clock = std::chrono::steady_clock;

/// Flips locked to the display's vblanks (driver.cpp): precise pacing on and BB_VBLANK_PACING
/// not 0.
bool VblankPacing();

/// The display's own vblanks (Windows: a thread of its own waits for them with
/// D3DKMTWaitForVerticalBlankEvent on the display `device`, such as \\.\DISPLAY1; elsewhere
/// nothing). Called again when the window moves to another display or its mode changes: the
/// watch starts over. Nothing without VblankPacing().
void WatchVblank(const wchar_t* device);
struct VblankTime {
    Clock::time_point last;           ///< the last vblank (smoothed against wake-up delays)
    std::chrono::nanoseconds period;  ///< the measured refresh period
};
/// The display's vblank timing while its refresh is steady: a fixed refresh rate. With G-Sync or
/// FreeSync the refreshes follow the frames, and with no watch running there is none.
std::optional<VblankTime> Vblank();

/// BB_PRECISE_PACING: on by default on Windows, off on Linux.
bool Enabled();

/// Sleeps about `duration` on the thread's high resolution timer, without spinning. False when
/// interrupted (an APC with `alertable`).
bool Sleep(std::chrono::nanoseconds duration, bool alertable = false);

/// Returns at `deadline`. `exact`: the last stretch is spun (the margin the timer needed lately,
/// at most `max_spin`), so the return is within microseconds; otherwise only the timer is used.
void WaitUntil(Clock::time_point deadline, bool exact,
               std::chrono::nanoseconds max_spin = std::chrono::microseconds(1500));

/// Slots `period` apart. An event takes a slot (Taken); the next slot is one period later, unless
/// the event came more than a quarter period late: then the cadence restarts at the event, so a
/// late frame is not followed by a burst that catches up (with G-Sync/FreeSync each flip is a
/// refresh, and uneven flips are visible judder).
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
    /// An event at `now` may take the slot: at most a sixteenth of a period early.
    [[nodiscard]] bool Ready(Clock::time_point now) const {
        return now + period / 16 >= slot;
    }
    void Taken(Clock::time_point now) {
        slot = now - slot > period / 4 ? now + period : slot + period;
    }
    /// Ticks without an event: the next slot, restarted at `now` after a stall of over a period.
    void Tick(Clock::time_point now) {
        slot = now - slot > period ? now + period : slot + period;
    }
    /// Moves the slot to `offset` after the display's vblank nearest to it (by at most half a
    /// refresh), `vblank` being any vblank on the display's grid of `refresh`: the flip then has
    /// the rest of that refresh to reach the display and is shown at the next vblank. A cadence
    /// that only keeps its own period drifts against the display's: its flips cross vblanks now
    /// and then, and a frame stays up a refresh longer or shorter (judder at 60 FPS on 180 Hz).
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
} // namespace LanternPacing
