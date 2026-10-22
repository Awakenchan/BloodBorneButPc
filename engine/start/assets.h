// SPDX-License-Identifier: GPL-2.0-or-later
// The engine's own assets, made locally from the player's dump (was scripts/engine_assets.py,
// adapted from droogie/bbhost engine/dream_mirror_layout.cpp and gcn/container.cpp).
#pragma once
#include "common.h"

namespace start {

/// The verified 1.09 image identity for BB_ENGINE_IMAGE_SHA256; throws when it is not that one.
std::string VerifyExecutable(const fs::path& elf);
/// The Dream's layout with the mirror's stand-in moved (verified input and output).
fs::path PrepareMirror(const fs::path& game, const fs::path& elf, const fs::path& cache);
/// The native option pages' movie and messages (and the rebirth talk script), from `tool`.
fs::path PrepareMenuAssets(const fs::path& game, const fs::path& elf, const fs::path& cache, const fs::path& tool,
                           bool rebirth);

/// Ebrietas' trail effect replaced as Nexus mod 41 does, in each effect binder that is the 1.09
/// one (scripts/engine_assets.py prepare_effect_fixes): (game path, prepared file) pairs for the
/// runtime (BB_FILE_FIXES). A binder a mod changed is left alone, with a line in `notes`.
std::vector<std::pair<std::string, fs::path>> PrepareEffectFixes(const fs::path& game, const fs::path& cache,
                                                                 std::vector<std::string>& notes);

/// Runs a program, waiting for it; its output goes to `output` (null: discarded). The exit code.
int RunProgram(const fs::path& program, const std::vector<std::wstring>& arguments, std::string* output = nullptr,
               void* log = nullptr);
/// A command line for CreateProcess, each argument quoted as needed.
std::wstring CommandLine(const fs::path& program, const std::vector<std::wstring>& arguments);

} // namespace start
