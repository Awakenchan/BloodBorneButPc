// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <thread>
#ifndef _WIN32
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace BbThreads {

#ifdef _WIN32
unsigned Available();
void MakeBackground();
void MakeCritical(bool bulk = false);
unsigned CapSpinUs(unsigned wanted);
#else
inline void MakeCritical(bool = false) {}
inline unsigned CapSpinUs(unsigned wanted) { return wanted; }
inline unsigned Available() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        return std::max(1, CPU_COUNT(&set));
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

inline void MakeBackground() {
    sched_param param{};
    if (sched_setscheduler(0, SCHED_IDLE, &param) != 0) {
        setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 19);
    }
}
#endif

}
