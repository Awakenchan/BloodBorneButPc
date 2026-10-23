// SPDX-License-Identifier: GPL-3.0-or-later
#include "shader_recompiler/native/tone_curve.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"

namespace Shader::Native {
namespace {

using IR::Opcode;

/// `value`'s instruction when it is one of `op`, else null.
IR::Inst* As(const IR::Value& value, Opcode op) {
    if (value.IsImmediate()) {
        return nullptr;
    }
    IR::Inst* inst = value.TryInst();
    return inst && inst->GetOpcode() == op ? inst : nullptr;
}

bool IsImm(const IR::Value& value, f32 wanted) {
    return value.IsImmediate() && value.Type() == IR::Type::F32 && value.F32() == wanted;
}

/// The constant buffer dword a value reads as a float (BitCastF32U32 of ReadConstBuffer), or -1.
s64 ConstWord(const IR::Value& value) {
    const IR::Inst* cast = As(value, Opcode::BitCastF32U32);
    if (!cast) {
        return -1;
    }
    const IR::Inst* read = As(cast->Arg(0), Opcode::ReadConstBuffer);
    if (!read || !read->Arg(1).IsImmediate()) {
        return -1;
    }
    return read->Arg(1).U32();
}

/// Whether `inst` is the two-operand `op` of `a` and `b` in either order; the matchers are
/// called with (operand, is it the first?) and must both hold.
template <class A, class B>
bool Both(const IR::Inst* inst, Opcode op, A&& a, B&& b) {
    if (!inst || inst->GetOpcode() != op) {
        return false;
    }
    return (a(inst->Arg(0)) && b(inst->Arg(1))) || (a(inst->Arg(1)) && b(inst->Arg(0)));
}

bool SameInst(const IR::Value& value, const IR::Inst* inst) {
    return !value.IsImmediate() && value.TryInst() == inst;
}

/// y = A * B * B with A = 1 - exp2(#1.442695 * (-x * k)), B = 1 + c * (#-1 + A).
bool MatchExponential(IR::Inst* clamp, ToneCurveMatch& m) {
    IR::Inst* product = As(clamp->Arg(0), Opcode::FPMul32);
    if (!product) {
        return false;
    }
    IR::Inst *a = nullptr, *b = nullptr;
    // product = B * (A * B), in any order.
    for (int i = 0; i < 2 && !a; ++i) {
        IR::Inst* candidate_b = As(product->Arg(i), Opcode::FPAdd32);
        IR::Inst* inner = As(product->Arg(1 - i), Opcode::FPMul32);
        if (!candidate_b || !inner) {
            continue;
        }
        for (int j = 0; j < 2; ++j) {
            if (SameInst(inner->Arg(j), candidate_b)) {
                if (IR::Inst* candidate_a = As(inner->Arg(1 - j), Opcode::FPSub32)) {
                    a = candidate_a;
                    b = candidate_b;
                }
            }
        }
    }
    if (!a || !IsImm(a->Arg(0), 1.0f)) {
        return false;
    }
    IR::Inst* exp = As(a->Arg(1), Opcode::FPExp2);
    if (!exp) {
        return false;
    }
    IR::Inst* exponent = As(exp->Arg(0), Opcode::FPMul32);
    IR::Inst* negated_scaled = nullptr;
    if (!Both(exponent, Opcode::FPMul32, [](const IR::Value& v) { return IsImm(v, 1.442695f); },
              [&](const IR::Value& v) { return (negated_scaled = As(v, Opcode::FPMul32)); })) {
        return false;
    }
    IR::Inst* scene = nullptr;
    IR::Value k;
    if (!Both(negated_scaled, Opcode::FPMul32,
              [&](const IR::Value& v) {
                  const IR::Inst* neg = As(v, Opcode::FPNeg32);
                  return neg && (scene = As(neg->Arg(0), Opcode::FPMax32));
              },
              [&](const IR::Value& v) { return ConstWord(v) >= 0 && (k = v, true); })) {
        return false;
    }
    if (!Both(scene, Opcode::FPMax32, [](const IR::Value& v) { return IsImm(v, 6.1035156e-05f); },
              [](const IR::Value&) { return true; })) {
        return false;
    }
    // B = (c * (#-1 + A)) + #1
    IR::Value c;
    const bool shoulder = Both(
        b, Opcode::FPAdd32, [](const IR::Value& v) { return IsImm(v, 1.0f); },
        [&](const IR::Value& v) {
            return Both(As(v, Opcode::FPMul32), Opcode::FPMul32,
                        [&](const IR::Value& w) { return ConstWord(w) >= 0 && (c = w, true); },
                        [&](const IR::Value& w) {
                            return Both(As(w, Opcode::FPAdd32), Opcode::FPAdd32,
                                        [](const IR::Value& u) { return IsImm(u, -1.0f); },
                                        [&](const IR::Value& u) { return SameInst(u, a); });
                        });
        });
    if (!shoulder) {
        return false;
    }
    m = ToneCurveMatch{.family = ToneCurveMatch::Family::Exponential,
                       .result = clamp,
                       .product = product,
                       .scene = scene,
                       .exponent = exponent,
                       .k = k,
                       .shoulder = c,
                       .k_word = u32(ConstWord(k)),
                       .shoulder_word = u32(ConstWord(c))};
    return true;
}

/// y = (k*x + (k*x) * (w * (k*x))) * recip(k*x + #1), x = max(#6.1035156e-05, s).
bool MatchReinhard(IR::Inst* clamp, ToneCurveMatch& m) {
    IR::Inst* product = As(clamp->Arg(0), Opcode::FPMul32);
    IR::Inst *numerator = nullptr, *recip = nullptr;
    if (!Both(product, Opcode::FPMul32,
              [&](const IR::Value& v) { return (numerator = As(v, Opcode::FPAdd32)); },
              [&](const IR::Value& v) { return (recip = As(v, Opcode::FPRecip32)); })) {
        return false;
    }
    IR::Inst* scene = nullptr;
    IR::Value k;
    s64 k_word = -1;
    // k * x, the same k and x everywhere.
    const auto kx = [&](const IR::Value& v) {
        IR::Inst* scene_here = nullptr;
        IR::Value k_here;
        const bool is = Both(As(v, Opcode::FPMul32), Opcode::FPMul32,
                             [&](const IR::Value& w) { return (scene_here = As(w, Opcode::FPMax32)); },
                             [&](const IR::Value& w) { return ConstWord(w) >= 0 && (k_here = w, true); });
        if (!is || (scene && scene_here != scene) || (k_word >= 0 && ConstWord(k_here) != k_word)) {
            return false;
        }
        scene = scene_here;
        k = k_here;
        k_word = ConstWord(k_here);
        return true;
    };
    IR::Inst* exponent = nullptr;
    if (!Both(As(recip->Arg(0), Opcode::FPAdd32), Opcode::FPAdd32,
              [&](const IR::Value& v) { return kx(v) && (exponent = v.Inst()); },
              [](const IR::Value& v) { return IsImm(v, 1.0f); })) {
        return false;
    }
    IR::Value w;
    const bool shape = Both(
        numerator, Opcode::FPAdd32, kx, [&](const IR::Value& v) {
            return Both(As(v, Opcode::FPMul32), Opcode::FPMul32, kx, [&](const IR::Value& u) {
                return Both(As(u, Opcode::FPMul32), Opcode::FPMul32,
                            [&](const IR::Value& t) { return ConstWord(t) >= 0 && (w = t, true); },
                            kx);
            });
        });
    if (!shape || !Both(scene, Opcode::FPMax32,
                        [](const IR::Value& v) { return IsImm(v, 6.1035156e-05f); },
                        [](const IR::Value&) { return true; })) {
        return false;
    }
    m = ToneCurveMatch{.family = ToneCurveMatch::Family::Reinhard,
                       .result = clamp,
                       .product = product,
                       .scene = scene,
                       .exponent = exponent,
                       .k = k,
                       .shoulder = w,
                       .k_word = u32(k_word),
                       .shoulder_word = u32(ConstWord(w))};
    return true;
}

} // namespace

std::vector<ToneCurveMatch> FindToneCurves(const IR::Program& program) {
    std::vector<ToneCurveMatch> matches;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != Opcode::FPSaturate32) {
                continue;
            }
            ToneCurveMatch m;
            if (MatchExponential(&inst, m) || MatchReinhard(&inst, m)) {
                matches.push_back(m);
            }
        }
    }
    return matches;
}

bool IsComposite(const std::vector<ToneCurveMatch>& matches) {
    if (matches.size() != 3) {
        return false;
    }
    std::set<const IR::Inst*> scenes;
    for (const auto& m : matches) {
        if (m.family != matches[0].family || m.k_word != matches[0].k_word ||
            m.shoulder_word != matches[0].shoulder_word) {
            return false;
        }
        scenes.insert(m.scene);
    }
    return scenes.size() == 3;
}

bool ToneCurveEnabled() {
    if (tone_curve_lab_override >= 0) {
        return tone_curve_lab_override != 0;
    }
    static const bool enabled = [] {
        const char* env = std::getenv("BB_HDR_SCENE_CURVE");
        const bool on = env && env[0] == '1';
        if (on) {
            std::printf("Native shaders: the composite's tone curve extended above its knee for "
                        "HDR (BB_HDR_SCENE_CURVE=1; shader caches kept apart)\n");
        }
        return on;
    }();
    return enabled;
}

bool ExtendToneCurve(IR::Program& program) {
    if (program.info.sw_stage != SwStage::Fragment) {
        return false;
    }
    auto matches = FindToneCurves(program);
    if (!IsComposite(matches)) {
        return false;
    }
    // Branch-free composites: the curve and everything it reaches in one block.
    IR::Block& block = *matches[0].result->GetParent();
    for (const auto& m : matches) {
        if (m.result->GetParent() != &block) {
            return false;
        }
    }
    // Program order: the first channel's clamp is where the shared values go.
    std::map<const IR::Inst*, size_t> order;
    size_t index = 0;
    for (IR::Inst& inst : block.Instructions()) {
        order[&inst] = index++;
    }
    std::ranges::sort(matches, [&](const auto& a, const auto& b) {
        return order.at(a.result) < order.at(b.result);
    });

    // The clamps the curve's values reach on their way to the outputs (before any change), with
    // the channels that reach each.
    std::map<IR::Inst*, std::set<size_t>> later_clamps;
    for (size_t channel = 0; channel < matches.size(); ++channel) {
        std::vector<IR::Inst*> work{matches[channel].result};
        std::set<IR::Inst*> seen;
        while (!work.empty()) {
            IR::Inst* inst = work.back();
            work.pop_back();
            for (const IR::Use& use : inst->Uses()) {
                IR::Inst* user = use.user;
                if (!seen.insert(user).second) {
                    continue;
                }
                if (user->GetParent() != &block) {
                    return false;
                }
                if (user->GetOpcode() == Opcode::FPSaturate32) {
                    later_clamps[user].insert(channel);
                }
                work.push_back(user);
            }
        }
    }

    using F32 = IR::F32;
    const auto first_at = IR::Block::InstructionList::s_iterator_to(*matches[0].result);
    IR::IREmitter ir{block, std::next(first_at)};
    const auto f = [](const IR::Value& v) { return F32{v}; };
    const auto imm = [&](f32 v) { return ir.Imm32(v); };
    const auto add = [&](F32 a, F32 b) { return f(ir.FPAdd(a, b)); };
    const auto sub = [&](F32 a, F32 b) { return f(ir.FPSub(a, b)); };
    const auto mul = [&](F32 a, F32 b) { return f(ir.FPMul(a, b)); };
    const auto div = [&](F32 a, F32 b) { return f(ir.FPDiv(a, b)); };

    // Parameters (push data).
    const F32 knee = f(ir.FPClamp(ir.ToneCurveParam(0), imm(0.05f), imm(0.99f)));
    const F32 peak = ir.ToneCurveParam(1);
    const F32 strength = f(ir.FPClamp(ir.ToneCurveParam(2), imm(0.0f), imm(1.0f)));
    const IR::U1 on = ir.LogicalAnd(ir.FPGreaterThan(ir.ToneCurveParam(3), imm(0.5f)),
                                    ir.FPGreaterThan(peak, imm(1.0f)));
    const F32 top = ir.FPLog2(f(ir.FPMax(peak, imm(1.0f))));
    const F32 start = mul(top, imm(float(ToneCurveRollOff) / 100.0f));
    const F32 room = f(ir.FPMax(sub(top, start), imm(1e-6f)));

    // The knee in the curve's units (z_k, increasing with the scene), the curve's value there
    // (log2) and its slope in log2-log2 (g): where the extension starts, continuous in value and
    // slope.
    const auto& m0 = matches[0];
    const F32 k = F32{m0.k};
    const F32 shoulder = F32{m0.shoulder};
    F32 z_knee, log_knee, slope;
    if (m0.family == ToneCurveMatch::Family::Exponential) {
        // (1 - e) * (1 - c e)^2 = knee for e = exp(-u); Newton from the c = 0 answer.
        F32 e = sub(imm(1.0f), knee);
        for (int i = 0; i < 5; ++i) {
            const F32 b = sub(imm(1.0f), mul(shoulder, e));
            const F32 a = sub(imm(1.0f), e);
            const F32 value = sub(mul(a, mul(b, b)), knee);
            const F32 derivative = f(ir.FPNeg(add(mul(b, b), mul(imm(2.0f), mul(shoulder, mul(a, b))))));
            e = f(ir.FPClamp(sub(e, div(value, derivative)), imm(1e-7f), imm(1.0f - 1e-6f)));
        }
        const F32 a = sub(imm(1.0f), e);
        const F32 b = sub(imm(1.0f), mul(shoulder, e));
        const F32 y = mul(a, mul(b, b));
        // u = -ln e; the shader's own coordinate is the exp2 argument, -u * log2(e).
        const F32 u = mul(f(ir.FPNeg(ir.FPLog2(e))), imm(0.6931472f));
        z_knee = f(ir.FPNeg(ir.FPLog2(e)));
        log_knee = ir.FPLog2(y);
        slope = div(mul(u, mul(e, mul(b, add(b, mul(imm(2.0f), mul(shoulder, a)))))), y);
    } else {
        // v (1 + w v) / (1 + v) = knee: w v^2 + (1 - knee) v - knee = 0.
        const F32 w = f(ir.FPMax(shoulder, imm(0.0f)));
        const F32 one_minus = sub(imm(1.0f), knee);
        const F32 root = ir.FPSqrt(add(mul(one_minus, one_minus), mul(imm(4.0f), mul(w, knee))));
        const F32 quadratic = div(sub(root, one_minus), f(ir.FPMax(mul(imm(2.0f), w), imm(1e-30f))));
        const F32 linear = div(knee, one_minus);
        const F32 v = F32{ir.Select(ir.FPGreaterThan(w, imm(1e-12f)), quadratic, linear)};
        const F32 one_plus = add(imm(1.0f), v);
        const F32 y = div(mul(v, add(imm(1.0f), mul(w, v))), one_plus);
        const F32 dy = div(sub(mul(add(imm(1.0f), mul(imm(2.0f), mul(w, v))), one_plus),
                               mul(v, add(imm(1.0f), mul(w, v)))),
                           mul(one_plus, one_plus));
        z_knee = v;
        log_knee = ir.FPLog2(y);
        slope = div(mul(v, dy), y);
    }
    (void)k;

    // Per channel: the extension, and the select.
    std::vector<IR::U1> above(matches.size());
    std::vector<F32> extended(matches.size());
    for (size_t channel = 0; channel < matches.size(); ++channel) {
        const auto& m = matches[channel];
        IR::IREmitter at{block, std::next(IR::Block::InstructionList::s_iterator_to(*m.result))};
        if (channel == 0) {
            at = ir; // after the shared values
        }
        const auto F = [](const IR::Value& v) { return F32{v}; };
        const F32 z = m.family == ToneCurveMatch::Family::Exponential
                          ? F(at.FPNeg(F32{IR::Value{m.exponent}}))
                          : F32{IR::Value{m.exponent}};
        above[channel] = at.LogicalAnd(on, at.FPGreaterThan(z, z_knee));
        // Linear in log2 from the knee with the curve's slope there...
        const F32 stops = at.FPLog2(F(at.FPDiv(F(at.FPMax(z, at.Imm32(1e-30f))), z_knee)));
        const F32 linear = F(at.FPAdd(log_knee, F(at.FPMul(slope, stops))));
        // ...to `ToneCurveRollOff` of the stops to the peak, then towards the peak.
        const F32 over = F(at.FPDiv(F(at.FPSub(linear, start)), room));
        const F32 rolled = F(at.FPAdd(
            start, F(at.FPMul(room, F(at.FPSub(at.Imm32(1.0f),
                                                at.FPExp2(F(at.FPMul(F(at.FPNeg(over)),
                                                                      at.Imm32(1.442695f))))))))));
        const F32 target = F32{at.Select(at.FPGreaterThan(linear, start), rolled, linear)};
        // Strength: the share of the extension in log2 against the game's curve (as shown, so at
        // most 1: Reinhard's grows past its white without a bound). Both meet at the knee.
        const F32 game = at.FPLog2(F(at.FPMax(F32{IR::Value{m.result}}, at.Imm32(1e-30f))));
        const F32 mixed = F(at.FPAdd(game, F(at.FPMul(strength, F(at.FPSub(target, game))))));
        extended[channel] = F32{at.Select(above[channel], at.FPExp2(mixed),
                                          F32{IR::Value{m.result}})};
    }
    // The game's uses of each channel's clamp (`order` holds the game's instructions) take the
    // select instead.
    for (size_t channel = 0; channel < matches.size(); ++channel) {
        IR::Inst* clamp = matches[channel].result;
        IR::Inst* select = extended[channel].Inst();
        std::vector<IR::Use> uses(clamp->Uses().begin(), clamp->Uses().end());
        for (const IR::Use& use : uses) {
            if (!order.contains(use.user)) {
                continue; // ours
            }
            IR::Value replacement = IR::Value{select};
            if (use.user->GetOpcode() == Opcode::FPNeg32) {
                // A screen blend (a + g - a g): above 1 the glow no longer darkens the highlight.
                IR::IREmitter before{block, IR::Block::InstructionList::s_iterator_to(*use.user)};
                replacement = before.FPMin(F32{IR::Value{select}}, before.Imm32(1.0f));
            }
            use.user->SetArg(use.operand, replacement);
        }
    }
    // The later clamps: to the peak where a channel reaching them is above the knee, else to 1
    // (the same FClamp to 1: bit for bit).
    for (const auto& [clamp, channels] : later_clamps) {
        IR::IREmitter before{block, IR::Block::InstructionList::s_iterator_to(*clamp)};
        IR::U1 any = before.Imm1(false);
        for (const size_t channel : channels) {
            any = before.LogicalOr(any, above[channel]);
        }
        const IR::Value upper = before.Select(any, before.Imm32(3.0e38f), before.Imm32(1.0f));
        const IR::Value value = clamp->Arg(0);
        clamp->ReplaceOpcode(Opcode::FPClamp32);
        clamp->SetArg(0, value);
        clamp->SetArg(1, before.Imm32(0.0f));
        clamp->SetArg(2, upper);
    }
    program.info.uses_tone_curve = true;
    if (tone_curve_lab_channels) {
        tone_curve_lab_channels->clear();
        for (const F32& value : extended) {
            tone_curve_lab_channels->push_back(value.Inst());
        }
    }
    return true;
}

} // namespace Shader::Native
