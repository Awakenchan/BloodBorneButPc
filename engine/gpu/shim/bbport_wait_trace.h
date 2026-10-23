// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>

namespace BbWaitTrace {
bool Enabled();

class Scope {
public:
    Scope();
    ~Scope();

private:
    void* frames[10];
    int depth = 0;
    std::chrono::steady_clock::time_point start;
};

void Report(double window_s);
}
