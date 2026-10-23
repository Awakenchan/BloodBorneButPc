// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: the game's tone curve in its final composite pixel shaders, found in the IR and extended
// above a knee for HDR (docs/hdr-scene-path.md, steps 1 and 2; docs/native_shader_compiler.md).
//
// The composites apply the curve per channel to the scene (bloom added) clamped below at 2^-14:
//   exponential:        y = A * B * B,  A = 1 - exp2(-1.442695 * k * x),  B = 1 + c * (A - 1)
//                       (k, c: constant buffer words 832, 833)
//   extended Reinhard:  y = v * (1 + w * v) / (1 + v),  v = k * x
//                       (k, w: constant buffer words 833, 834)
// each followed by a clamp to [0, 1] (FPSaturate32). FindToneCurves recognises them by that IR
// shape; a composite has one per colour channel.
//
// ExtendToneCurve (off unless BB_HDR_SCENE_CURVE=1) replaces each channel's clamped curve by a
// select: the game's value below the knee (its own instructions: bit for bit), above it an
// extension continuous in value and slope that rolls off towards the peak over paper white, as
// native mode's gain does. The clamps after the curve on its way to the colour output stop at the
// peak instead of 1 only where a channel is above the knee. Its parameters are push data
// (ToneCurvePush), so they change live.
#pragma once

#include <vector>
#include "common/types.h"
#include "shader_recompiler/ir/value.h"

namespace Shader::IR {
struct Program;
}

namespace Shader::Native {

struct ToneCurveMatch {
    enum class Family : u8 { Exponential, Reinhard };
    Family family{};
    IR::Inst* result{};  ///< the clamp: FPSaturate32 of `product`
    IR::Inst* product{}; ///< the curve before the clamp
    IR::Inst* scene{};   ///< FPMax32(#6.1035156e-05, scene value)
    /// The scene value in the curve's own units, increasing with it: exponential: the exp2
    /// argument (-1.442695 * k * x, so it decreases: negated when used); Reinhard: k * x.
    IR::Inst* exponent{};
    IR::Value k;         ///< the curve's scale (F32)
    IR::Value shoulder;  ///< exponential: c; Reinhard: w (F32)
    u32 k_word{};        ///< constant buffer dwords of k and the shoulder
    u32 shoulder_word{};
};

/// Every tone curve channel in `program` (any stage; the composites have three).
std::vector<ToneCurveMatch> FindToneCurves(const IR::Program& program);

/// Whether the curves in `matches` are one composite's: three channels, one family, the same
/// constants, three scene values.
bool IsComposite(const std::vector<ToneCurveMatch>& matches);

/// BB_HDR_SCENE_CURVE=1 (read once): composites are compiled with the extension. Shader caches
/// then live beside the usual ones (cache_storage.cpp), so turning it on or off never mixes them.
bool ToneCurveEnabled();

/// Extends a composite's curve in place (a fragment program in one block). Returns whether it
/// did; sets Info::uses_tone_curve.
bool ExtendToneCurve(IR::Program& program);

/// bb-shader-lab: overrides BB_HDR_SCENE_CURVE for this thread (-1: the variable decides), and
/// when set receives each channel's new value (the select) in the clamps' program order.
inline thread_local int tone_curve_lab_override = -1;
inline thread_local std::vector<IR::Inst*>* tone_curve_lab_channels = nullptr;

/// The extension's parameters, pushed after Shader::PushData by pipelines whose fragment shader
/// uses them (Info::uses_tone_curve).
struct ToneCurvePush {
    float knee;     ///< the curve's output (0..1, SDR) where the extension starts
    float peak;     ///< peak / paper white: the most a channel reaches (SDR white = 1)
    float strength; ///< share of the extension in log2 (0: the game's curve, 1: all of it)
    float enabled;  ///< 0: the game's curve everywhere (bit for bit)
};
inline constexpr u32 ToneCurvePushOffset = 128;
inline constexpr u32 ToneCurveRollOff = 70; ///< percent of the stops to the peak kept linear

} // namespace Shader::Native
