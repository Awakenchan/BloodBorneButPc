// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common.h"

namespace start {

struct GameCheckError : Error {
    using Error::Error;
};

void PrepareImage(const fs::path& game, const fs::path& out, const fs::path& engine);

}
