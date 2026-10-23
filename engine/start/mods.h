// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common.h"

namespace start {

fs::path BuildModsOverlay(const fs::path& game, const fs::path& out, const fs::path& mods_dir, const fs::path& config,
                          bool enabled);
void RemoveOverlay(const fs::path& path);

}
