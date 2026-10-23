// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: hardware interpolation where the shader cannot tell the difference. On NVIDIA every
// pixel shader input is read per vertex (VK_KHR_fragment_shader_barycentric) and interpolated by
// GCN's own two steps, v_interp_p1/p2: P0 + I * (P1 - P0) + J * (P2 - P0) (Profile::
// needs_manual_interpolation, shadPS4 PR 1644). That keeps integers passed as float varyings
// exact (Bloodborne's instanced objects truncate their instance index; NVIDIA's interpolation of
// three equal values is not always exact, and the objects flickered). It costs 8.7% of the pixel
// shaders' code and 5.6% of their registers on an RTX 5060 (docs/native_shader_compiler.md):
// three attribute reads and the arithmetic per component, where the hardware's interpolation is
// one instruction.
//
// This pass gives an attribute back to the hardware when every read of it is that two-step
// formula with the hardware's own barycentrics, and no use of the result needs its exact bits,
// followed through float arithmetic, selects, phis, bit casts and lane shuffles: a conversion
// to an integer, floor, ceil, trunc, fract, rounding, a comparison with an integer constant
// other than zero or with a computed value, or any other integer or boolean result. Image
// sampling, color exports (also as 16-bit floats), stores, comparisons with zero or with a
// constant between integers, and |a| against |b| (a cube face) end the walk. Anything else keeps
// the manual formula, as before. On the dev cache 880 of 1201 inputs go back to the hardware.

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {

namespace {

using Interpolation = Info::Interpolation;

/// The GetAttribute instruction behind `value`, or null.
IR::Inst* AttributeRead(const IR::Value& value) {
    IR::Inst* inst = value.IsImmediate() ? nullptr : value.TryInst();
    return inst && inst->GetOpcode() == IR::Opcode::GetAttribute ? inst : nullptr;
}

bool IsRead(const IR::Inst* inst, IR::Attribute attr, u32 comp, u32 index) {
    return inst && inst->Arg(0).Attribute() == attr && inst->Arg(1).U32() == comp &&
           inst->Arg(2).U32() == index;
}

bool HardwareBarycentric(IR::Attribute attr, Interpolation& interp) {
    switch (attr) {
    case IR::Attribute::BaryCoordSmooth:
        interp = {Qualifier::Smooth, Qualifier::None};
        return true;
    case IR::Attribute::BaryCoordSmoothCentroid:
        interp = {Qualifier::Smooth, Qualifier::Centroid};
        return true;
    case IR::Attribute::BaryCoordSmoothSample:
        interp = {Qualifier::Smooth, Qualifier::Sample};
        return true;
    case IR::Attribute::BaryCoordNoPersp:
        interp = {Qualifier::NoPerspective, Qualifier::None};
        return true;
    case IR::Attribute::BaryCoordNoPerspCentroid:
        interp = {Qualifier::NoPerspective, Qualifier::Centroid};
        return true;
    case IR::Attribute::BaryCoordNoPerspSample:
        interp = {Qualifier::NoPerspective, Qualifier::Sample};
        return true;
    default:
        return false; // the pull model and anything else stay manual
    }
}

/// A register write SSA has already read through (nothing reads registers after it), or a bit
/// cast that only feeds such writes: dead until dead code elimination removes it.
bool IsDeadWrite(const IR::Inst* inst) {
    switch (inst->GetOpcode()) {
    case IR::Opcode::SetVectorRegister:
    case IR::Opcode::SetScalarRegister:
        return true;
    case IR::Opcode::BitCastU32F32:
    case IR::Opcode::BitCastF32U32:
        return std::ranges::all_of(inst->Uses(),
                                   [](const IR::Use& use) { return IsDeadWrite(use.user); });
    default:
        return false;
    }
}

void DropDeadWrites(IR::Inst* inst) {
    const std::vector<IR::Use> uses(inst->Uses().begin(), inst->Uses().end());
    for (const IR::Use& use : uses) {
        DropDeadWrites(use.user);
        use.user->Invalidate();
    }
}

bool OnlyUsedBy(const IR::Inst* inst, std::initializer_list<const IR::Inst*> users) {
    for (const IR::Use& use : inst->Uses()) {
        if (std::find(users.begin(), users.end(), use.user) == users.end() &&
            !IsDeadWrite(use.user)) {
            return false;
        }
    }
    return true;
}

/// One v_interp_p1/p2 pair: P2 = FPFma(P2a - P0b, J, P1), P1 = FPFma(P1a - P0a, I, P0a).
struct Formula {
    IR::Inst* result;
    IR::Attribute param;
    u32 comp;
    Interpolation interp;
    std::array<IR::Inst*, 4> reads; ///< P0a, P1a, P0b, P2a
};

bool Match(IR::Inst* fma2, Formula& out) {
    if (fma2->GetOpcode() != IR::Opcode::FPFma32) {
        return false;
    }
    IR::Inst* const sub2 = fma2->Arg(0).IsImmediate() ? nullptr : fma2->Arg(0).TryInst();
    IR::Inst* const j = AttributeRead(fma2->Arg(1));
    IR::Inst* const fma1 = fma2->Arg(2).IsImmediate() ? nullptr : fma2->Arg(2).TryInst();
    if (!sub2 || !j || !fma1 || sub2->GetOpcode() != IR::Opcode::FPSub32 ||
        fma1->GetOpcode() != IR::Opcode::FPFma32) {
        return false;
    }
    IR::Inst* const sub1 = fma1->Arg(0).IsImmediate() ? nullptr : fma1->Arg(0).TryInst();
    IR::Inst* const i = AttributeRead(fma1->Arg(1));
    IR::Inst* const p0a = AttributeRead(fma1->Arg(2));
    if (!sub1 || !i || !p0a || sub1->GetOpcode() != IR::Opcode::FPSub32) {
        return false;
    }
    IR::Inst* const p1a = AttributeRead(sub1->Arg(0));
    IR::Inst* const p2a = AttributeRead(sub2->Arg(0));
    IR::Inst* const p0b = AttributeRead(sub2->Arg(1));
    const IR::Attribute param = p0a->Arg(0).Attribute();
    if (!IR::IsParam(param) || !p1a || !p2a || !p0b || AttributeRead(sub1->Arg(1)) != p0a) {
        return false;
    }
    const u32 comp = p0a->Arg(1).U32();
    const IR::Attribute bary = i->Arg(0).Attribute();
    if (!IsRead(p0a, param, comp, 0) || !IsRead(p1a, param, comp, 1) ||
        !IsRead(p0b, param, comp, 0) || !IsRead(p2a, param, comp, 2) || !IsRead(i, bary, 0, 0) ||
        !IsRead(j, bary, 1, 0) || !HardwareBarycentric(bary, out.interp)) {
        return false;
    }
    // The steps feed nothing but the next one.
    if (!OnlyUsedBy(fma1, {fma2}) || !OnlyUsedBy(sub1, {fma1}) || !OnlyUsedBy(sub2, {fma2}) ||
        !OnlyUsedBy(p0a, {sub1, fma1}) || !OnlyUsedBy(p1a, {sub1}) || !OnlyUsedBy(p0b, {sub2}) ||
        !OnlyUsedBy(p2a, {sub2})) {
        return false;
    }
    out.result = fma2;
    out.param = param;
    out.comp = comp;
    out.reads = {p0a, p1a, p0b, p2a};
    return true;
}

bool IsFloat(IR::Type type) {
    constexpr IR::Type floats = IR::Type::F16 | IR::Type::F32 | IR::Type::F64 | IR::Type::F16x2 |
                                IR::Type::F16x3 | IR::Type::F16x4 | IR::Type::F32x2 |
                                IR::Type::F32x3 | IR::Type::F32x4 | IR::Type::F64x2 |
                                IR::Type::F64x3 | IR::Type::F64x4;
    return (type & floats) != IR::Type::Void;
}

enum class UseKind { Exact, Follow, End };

bool IsAbs(const IR::Value& value) {
    const IR::Inst* inst = value.IsImmediate() ? nullptr : value.TryInst();
    return inst && inst->GetOpcode() == IR::Opcode::FPAbs32;
}

/// What a use of the value means. `bits`: the value is a float's bits moved as an integer (a
/// GCN register holds bits; SSA leaves the bit casts in between until they fold).
UseKind Classify(const IR::Inst* user, bool bits) {
    switch (user->GetOpcode()) {
    case IR::Opcode::Phi:
    case IR::Opcode::BitCastU32F32:
    case IR::Opcode::BitCastF32U32:
        return UseKind::Follow;
    case IR::Opcode::SetVectorRegister:
    case IR::Opcode::SetScalarRegister:
        return UseKind::End; // register writes SSA has already read through
    default:
        break;
    }
    if (bits) {
        switch (user->GetOpcode()) {
        case IR::Opcode::QuadBroadcast: // another lane's value, as exact as this one
        case IR::Opcode::Shuffle:
        case IR::Opcode::ShuffleXor:
            return UseKind::Follow;
        default:
            return user->Type() == IR::Type::Void ? UseKind::End : UseKind::Exact;
        }
    }
    switch (user->GetOpcode()) {
    case IR::Opcode::FPOrdEqual32:
    case IR::Opcode::FPUnordEqual32:
    case IR::Opcode::FPOrdNotEqual32:
    case IR::Opcode::FPUnordNotEqual32:
    case IR::Opcode::FPOrdLessThan32:
    case IR::Opcode::FPUnordLessThan32:
    case IR::Opcode::FPOrdGreaterThan32:
    case IR::Opcode::FPUnordGreaterThan32:
    case IR::Opcode::FPOrdLessThanEqual32:
    case IR::Opcode::FPUnordLessThanEqual32:
    case IR::Opcode::FPOrdGreaterThanEqual32:
    case IR::Opcode::FPUnordGreaterThanEqual32:
        // Against zero (GCN's legacy multiply asks this of every operand) or a constant between
        // integers: a value equal at the three vertices interpolates to that value or a bit off
        // it, which compares the same; zero at the three vertices is exactly zero either way. An
        // integer constant or a computed value could fall on the value: exact.
        for (size_t arg = 0; arg < 2; ++arg) {
            const IR::Value value = user->Arg(arg);
            if (value.IsImmediate() && value.Type() == IR::Type::F32 &&
                (value.F32() == 0.0f || value.F32() != std::trunc(value.F32()))) {
                return UseKind::End;
            }
        }
        // |a| against |b|: the cube face of a direction (SelectCubeResult). A direction on the
        // edge between two faces samples the texels the two faces share there.
        if (IsAbs(user->Arg(0)) && IsAbs(user->Arg(1))) {
            return UseKind::End;
        }
        return UseKind::Exact;
    case IR::Opcode::PackHalf2x16:
        // A color export's 16-bit floats (v_cvt_pkrtz_f16_f32): 11 bits, and the render target
        // rounds again; a value one bit off an integer stays within 2^-10 of it.
        return UseKind::End;
    case IR::Opcode::FPFloor32:
    case IR::Opcode::FPCeil32:
    case IR::Opcode::FPTrunc32:
    case IR::Opcode::FPFract32:
    case IR::Opcode::FPRoundEven32:
        return UseKind::Exact;
    case IR::Opcode::ImageSampleRaw:
    case IR::Opcode::ImageSampleImplicitLod:
    case IR::Opcode::ImageSampleExplicitLod:
    case IR::Opcode::ImageSampleDrefImplicitLod:
    case IR::Opcode::ImageSampleDrefExplicitLod:
    case IR::Opcode::ImageGather:
    case IR::Opcode::ImageGatherDref:
    case IR::Opcode::ImageGradient:
    case IR::Opcode::ImageQueryLod:
        return UseKind::End;
    default:
        break;
    }
    const IR::Type type = user->Type();
    if (type == IR::Type::Void) {
        return UseKind::End; // exports and stores
    }
    return IsFloat(type) ? UseKind::Follow : UseKind::Exact;
}

/// Whether the interpolated value reaches a use that needs GCN's exact bits.
bool NeedsExact(std::span<IR::Inst* const> values) {
    std::vector<std::pair<const IR::Inst*, bool>> stack;
    std::unordered_set<const IR::Inst*> seen;
    for (IR::Inst* const value : values) {
        stack.push_back({value, false});
        seen.insert(value);
    }
    while (!stack.empty()) {
        const auto [inst, bits] = stack.back();
        stack.pop_back();
        for (const IR::Use& use : inst->Uses()) {
            const IR::Inst* user = use.user;
            switch (Classify(user, bits)) {
            case UseKind::Exact:
                return true;
            case UseKind::Follow:
                if (seen.insert(user).second) {
                    if (seen.size() > 4096) {
                        return true; // too far to follow: stay exact
                    }
                    const IR::Opcode op = user->GetOpcode();
                    stack.push_back({user, op == IR::Opcode::BitCastU32F32 ||
                                               (op == IR::Opcode::Phi && bits)});
                }
                break;
            case UseKind::End:
                break;
            }
        }
    }
    return false;
}

} // namespace

void HardwareInterpolationPass(IR::Program& program, const RuntimeInfo& runtime_info) {
    Info& info = program.info;
    std::vector<Formula> formulas;
    // Per attribute: its live per-vertex reads, those inside a formula, and whether it can go back.
    std::array<u32, IR::NumParams> reads{}, covered{};
    std::array<bool, IR::NumParams> keep{};
    std::array<std::optional<Interpolation>, IR::NumParams> interp{};
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::GetAttribute && IR::IsParam(inst.Arg(0).Attribute()) &&
                inst.HasUses()) {
                ++reads[u32(inst.Arg(0).Attribute()) - u32(IR::Attribute::Param0)];
            }
            Formula f;
            if (Match(&inst, f)) {
                formulas.push_back(f);
            }
        }
    }
    std::array<std::vector<IR::Inst*>, IR::NumParams> results;
    for (const Formula& f : formulas) {
        const u32 n = u32(f.param) - u32(IR::Attribute::Param0);
        covered[n] += 4;
        if (interp[n] && (interp[n]->primary != f.interp.primary ||
                          interp[n]->auxiliary != f.interp.auxiliary)) {
            keep[n] = true; // read with two kinds of barycentrics
        }
        interp[n] = f.interp;
        results[n].push_back(f.result);
    }
    // Two inputs of one vertex output (one location, two interpolations: invalid SPIR-V that 30
    // of a player's 841 captures hold, with or without this pass) stay as they were.
    const auto& inputs = runtime_info.hw.fs.inputs;
    const u32 num_inputs = std::min<u32>(runtime_info.hw.fs.num_inputs, IR::NumParams);
    for (u32 n = 0; n < num_inputs; ++n) {
        for (u32 other = 0; other < num_inputs; ++other) {
            if (other != n && !inputs[other].IsDefault() &&
                inputs[other].param_index == inputs[n].param_index) {
                keep[n] = true;
            }
        }
    }
    // An attribute read some other way too stays manual; the others' values are followed once.
    for (u32 n = 0; n < IR::NumParams; ++n) {
        keep[n] = keep[n] || covered[n] != reads[n] ||
                  info.fs_interpolation[n].primary != Qualifier::PerVertex ||
                  (!results[n].empty() && NeedsExact(results[n]));
    }
    for (const Formula& f : formulas) {
        const u32 n = u32(f.param) - u32(IR::Attribute::Param0);
        if (keep[n]) {
            continue;
        }
        IR::Block* const block = f.result->GetParent();
        IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(*f.result)};
        IR::Inst* const fma1 = f.result->Arg(2).Inst();
        IR::Inst* const sub1 = fma1->Arg(0).Inst();
        IR::Inst* const sub2 = f.result->Arg(0).Inst();
        f.result->ReplaceUsesWithAndRemove(ir.GetAttribute(f.param, f.comp));
        // The per-vertex reads go now: none may reach the SPIR-V of a smooth input.
        for (IR::Inst* const dead : {fma1, sub1, sub2}) {
            DropDeadWrites(dead);
            dead->Invalidate();
        }
        for (IR::Inst* const read : f.reads) {
            DropDeadWrites(read);
            read->Invalidate();
        }
    }
    for (u32 n = 0; n < IR::NumParams; ++n) {
        if (interp[n] && !keep[n]) {
            info.fs_interpolation[n] = *interp[n];
        }
    }
}

} // namespace Shader::Optimization
