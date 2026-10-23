// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace BbUsage {

struct Sample {
    float cpu = -1.0f;
    float gpu = -1.0f;
};
Sample Get(bool cpu, bool gpu);

}
