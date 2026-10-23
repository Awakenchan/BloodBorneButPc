// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>

namespace MouseTurn {

inline float DegreesPerCount(int sensitivity) {
    const int s = std::clamp(sensitivity, -10, 20);
    if (s >= 0) return 0.2f * (0.5f + 0.15f * float(s));
    float degrees = 0.1f;
    for (int i = 0; i < -s; ++i) degrees *= 0.8f;
    return degrees;
}

struct Settings {
    float degrees_per_count = 0.25f;
    bool invert_x = false, invert_y = false;
};

inline bool Turn(float dx, float dy, const Settings& s, float& pitch, float& yaw, float lowest, float highest) {
    if (dx == 0.0f && dy == 0.0f) return false;
    constexpr float kRadiansPerDegree = 0.0174532924f;
    constexpr float kPi = 3.14159274f;
    const float right = (s.invert_x ? -dx : dx) * s.degrees_per_count;
    const float down = (s.invert_y ? -dy : dy) * s.degrees_per_count;

    pitch += down * kRadiansPerDegree;
    pitch = pitch < lowest ? lowest : std::min(highest, pitch);
    yaw += right * kRadiansPerDegree;

    for (int i = 0; i < 64 && yaw > kPi; ++i) yaw -= 2 * kPi;
    for (int i = 0; i < 64 && yaw < -kPi; ++i) yaw += 2 * kPi;
    return true;
}

}
