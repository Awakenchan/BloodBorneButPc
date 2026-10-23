// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: shader capture (BB_SHADER_CAPTURE=1). Every shader the game draws or dispatches with is
// written once per distinct runtime state, with all a compile reads: the GCN code, the stage's
// user data, the RuntimeInfo, the fetch shader's code and the device's Profile. The shader lab
// (tools/shader_lab.cpp) compiles the captures offline with the current recompiler and the native
// one and compares them, without the game.
#pragma once

#include <span>
#include "common/types.h"

namespace Shader {
struct Info;
struct RuntimeInfo;
struct Profile;
} // namespace Shader

namespace Shader::Native {

/// The capture file's layout (little endian), followed by RuntimeInfo (runtime_info_size bytes),
/// the user data (user_data_count dwords), the code (code_dwords), the fetch shader's code
/// (fetch_dwords; its address is user data dwords fetch_sgpr and fetch_sgpr + 1) and the flattened
/// user data the resource table walker read from the game's memory for this draw (flat_dwords;
/// the compile reads sharps from it). Version 3 then holds the vertex attributes' V#s the compile
/// reads through table pointers in the user data: a u32 count, then per V# its pointer's SGPR,
/// its dword offset in that table and its four dwords (CaptureSharp). Version 4 then holds the
/// tessellation constants hull and domain shaders read while they compile (CaptureTess, then
/// its bytes; size 0 for other shaders). Version 5 ends a geometry shader's state with the copy
/// shader its compile reads (RuntimeInfo's hw.gs.vs_copy, game code): its dwords, then their count
/// and CopyTrailerMagic, read from the end of the file.
struct CaptureHeader {
    char magic[4];
    u32 version;
    u32 hw_stage;
    u32 sw_stage;
    u64 hash;
    u32 runtime_info_size;
    u32 user_data_count;
    u32 code_dwords;
    u32 fetch_dwords;
    u32 fetch_sgpr;
    u32 flat_dwords;
};
inline constexpr char CaptureMagic[4] = {'B', 'B', 'S', '1'};
inline constexpr u32 CaptureVersion = 5;
inline constexpr u32 CopyTrailerMagic = 0x50435356; // "VSCP"

struct CaptureSharp {
    u32 sgpr_base;
    u32 dword_offset;
    u32 words[4];
};

/// The tessellation constants' V# (its table pointer's SGPR, NumScalarRegs when it sits in the
/// user data itself) and the size of the constants that follow.
struct CaptureTess {
    u32 size;
    u32 sgpr_base;
    u32 dword_offset;
    u32 words[4];
};

/// Whether BB_SHADER_CAPTURE=1 (read once).
bool CaptureEnabled();

/// bbport: shader states. Each shader permutation the cache holds also keeps its compile inputs
/// (the capture format) beside it in the cache folder, "<pgm_hash>_<perm>.bbs" as its ".spv": when
/// the shader compiler changes, bb-start has bb-shader-lab recompile the cache from them before
/// the game starts (tests/shader_lab.cpp --recompile-cache) instead of every shader compiling
/// again in play. On unless BB_SHADER_STATES=0.
inline constexpr const char* StateExtension = ".bbs";
bool StatesEnabled();

/// Keeps the compile inputs of this program's permutation `perm_idx` if the cache folder has none
/// yet (once per session per permutation). Thread safe; reads guest memory like Capture.
void StoreState(const Info& info, const RuntimeInfo& runtime_info, std::span<const u32> code,
                size_t perm_idx);

/// Writes the shader's capture if this shader and runtime state were not written yet. `info` must
/// hold this draw's user data, the program's fetch shader fields and this draw's flattened user
/// data (after RefreshFlatBuf). Thread safe.
void Capture(const Info& info, const RuntimeInfo& runtime_info, std::span<const u32> code,
             const Profile& profile);

} // namespace Shader::Native
