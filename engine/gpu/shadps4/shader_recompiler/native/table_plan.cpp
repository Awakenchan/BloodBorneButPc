// SPDX-License-Identifier: GPL-3.0-or-later
#include "shader_recompiler/native/table_plan.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <set>
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/ir/opcodes.h"
#include "shader_recompiler/ir/passes/srt.h"
#include "shader_recompiler/ir/reg.h"

namespace Shader::Native {
namespace {

/// The place a table read was given in the flattened user data (0: none, it reads memory).
u32 FlatOffset(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::ReadConst:
        return inst.Flags<u16>();
    case IR::Opcode::ReadConstBuffer:
        return u32(inst.Flags<IR::BufferInstInfo>().flatbuf_off_dw.Value());
    default:
        return 0;
    }
}

struct Compiler {
    std::vector<TablePlan::ExprOp>& ops;
    u32 dst;
    u32 depth = 0;
    u32 max_depth = 0;
    std::string error;

    void Push(TablePlan::Op op, u32 value = 0) {
        ops.push_back({op, value});
        if (op == TablePlan::Op::Imm || op == TablePlan::Op::Flat) {
            max_depth = std::max(max_depth, ++depth);
        } else if (op != TablePlan::Op::Not) {
            depth -= op == TablePlan::Op::Bfe ? 2 : 1;
        }
    }

    bool Value(const IR::Value& value) {
        if (value.IsImmediate()) {
            Push(TablePlan::Op::Imm, value.U32());
            return true;
        }
        const IR::Inst* inst = value.Inst();
        using IR::Opcode;
        const auto args = [&](size_t count, TablePlan::Op op) {
            for (size_t i = 0; i < count; ++i) {
                if (!Value(inst->Arg(i))) {
                    return false;
                }
            }
            Push(op);
            return true;
        };
        switch (inst->GetOpcode()) {
        case Opcode::GetUserData:
            Push(TablePlan::Op::Flat, static_cast<u32>(inst->Arg(0).ScalarReg()));
            return true;
        case Opcode::ReadConst:
        case Opcode::ReadConstBuffer: {
            const u32 at = FlatOffset(*inst);
            if (at == 0 || at >= dst) {
                error = "offset from a value not flattened before it";
                return false;
            }
            Push(TablePlan::Op::Flat, at);
            return true;
        }
        case Opcode::IAdd32:
            return args(2, TablePlan::Op::Add);
        case Opcode::ISub32:
            return args(2, TablePlan::Op::Sub);
        case Opcode::IMul32:
            return args(2, TablePlan::Op::Mul);
        case Opcode::ShiftLeftLogical32:
            return args(2, TablePlan::Op::Shl);
        case Opcode::ShiftRightLogical32:
            return args(2, TablePlan::Op::Shr);
        case Opcode::BitwiseAnd32:
            return args(2, TablePlan::Op::And);
        case Opcode::BitwiseOr32:
            return args(2, TablePlan::Op::Or);
        case Opcode::BitwiseXor32:
            return args(2, TablePlan::Op::Xor);
        case Opcode::BitwiseNot32:
            return args(1, TablePlan::Op::Not);
        case Opcode::UMin32:
            return args(2, TablePlan::Op::UMin);
        case Opcode::UMax32:
            return args(2, TablePlan::Op::UMax);
        case Opcode::BitFieldUExtract:
            return args(3, TablePlan::Op::Bfe);
        default:
            error = std::string("offset computed by ") + std::string(IR::NameOf(inst->GetOpcode()));
            return false;
        }
    }
};

} // namespace

void TablePlan::Build(const IR::Program& program) {
    entries.clear();
    ops.clear();
    table_dwords.clear();
    error.clear();
    valid = false;
    roots = tables = depth = dynamic = 0;
    flat_dwords = program.info.srt_info.flattened_bufsize_dw;
    if (flat_dwords < FirstFlat) {
        error = "flattened user data smaller than the user data";
        return;
    }
    entries.assign(flat_dwords - FirstFlat, Entry{});
    std::vector<bool> filled(entries.size());
    std::vector<std::vector<ExprOp>> exprs(entries.size());
    for (const IR::Block* block : program.blocks) {
        for (const IR::Inst& inst : block->Instructions()) {
            const u32 dst = FlatOffset(inst);
            if (dst == 0) {
                continue;
            }
            if (dst < FirstFlat || dst >= flat_dwords) {
                error = "table read outside the flattened user data";
                return;
            }
            const IR::Inst* base = inst.Arg(0).TryInst();
            const IR::Inst* lo = base ? base->Arg(0).TryInst() : nullptr;
            if (!lo) {
                error = "table pointer not from the user data or a table";
                return;
            }
            u32 parent = 0;
            if (lo->GetOpcode() == IR::Opcode::GetUserData) {
                parent = static_cast<u32>(lo->Arg(0).ScalarReg());
                if (parent >= FirstFlat) {
                    error = "table pointer in a register beyond the user data";
                    return;
                }
            } else {
                parent = FlatOffset(*lo);
                if (parent < FirstFlat || parent >= dst) {
                    error = "table pointer not flattened before the values below it";
                    return;
                }
            }
            Entry entry{.parent = u16(parent)};
            std::vector<ExprOp> expr;
            if (inst.Arg(1).IsImmediate()) {
                entry.offset = inst.Arg(1).U32();
            } else {
                Compiler compiler{expr, dst};
                if (!compiler.Value(inst.Arg(1))) {
                    error = compiler.error;
                    return;
                }
                if (compiler.max_depth > MaxStack) {
                    error = "offset expression too deep";
                    return;
                }
                entry.expr_len = u16(expr.size());
            }
            const u32 i = dst - FirstFlat;
            if (filled[i]) {
                // A duplicate the pass gave the same place: it must be the same read.
                const Entry& old = entries[i];
                const bool same_expr =
                    expr.size() == exprs[i].size() &&
                    std::equal(expr.begin(), expr.end(), exprs[i].begin(),
                               [](const ExprOp& a, const ExprOp& b) {
                                   return a.op == b.op && a.value == b.value;
                               });
                if (old.parent != entry.parent || old.expr_len != entry.expr_len ||
                    (entry.expr_len == 0 && old.offset != entry.offset) || !same_expr) {
                    error = "two different table reads given one place";
                    return;
                }
                continue;
            }
            filled[i] = true;
            entries[i] = entry;
            exprs[i] = std::move(expr);
        }
    }
    std::set<u32> parents;
    std::vector<u32> level(flat_dwords, 0);
    table_dwords.assign(flat_dwords, 0);
    for (u32 i = 0; i < entries.size(); ++i) {
        if (!filled[i]) {
            error = "a flattened value no table read fills";
            return;
        }
        Entry& entry = entries[i];
        if (entry.expr_len) {
            entry.offset = u32(ops.size());
            ops.insert(ops.end(), exprs[i].begin(), exprs[i].end());
            ++dynamic;
        }
        parents.insert(entry.parent);
        if (!entry.expr_len) {
            table_dwords[entry.parent] = std::max(table_dwords[entry.parent], entry.offset + 1);
        }
        roots += entry.parent < FirstFlat && level[entry.parent] == 0;
        level[entry.parent] = entry.parent < FirstFlat ? 1 : std::max(level[entry.parent], 1u);
        level[FirstFlat + i] = entry.parent < FirstFlat ? 1 : level[entry.parent] + 1;
        depth = std::max(depth, level[FirstFlat + i]);
    }
    tables = u32(parents.size());
    valid = true;
}

u32 TablePlan::Offset(const Entry& entry, const u32* flat) const {
    if (entry.expr_len == 0) {
        return entry.offset;
    }
    u32 stack[MaxStack];
    u32 top = 0;
    for (u32 i = entry.offset; i < entry.offset + entry.expr_len; ++i) {
        const ExprOp& op = ops[i];
        switch (op.op) {
        case Op::Imm:
            stack[top++] = op.value;
            continue;
        case Op::Flat:
            stack[top++] = flat[op.value];
            continue;
        case Op::Not:
            stack[top - 1] = ~stack[top - 1];
            continue;
        case Op::Bfe: {
            const u32 base = stack[top - 3], offset = stack[top - 2] & 31, count = stack[top - 1] & 31;
            top -= 2;
            stack[top - 1] = (base >> offset) & ((1u << count) - 1);
            continue;
        }
        default:
            break;
        }
        const u32 b = stack[--top];
        u32& a = stack[top - 1];
        switch (op.op) {
        case Op::Add:
            a += b;
            break;
        case Op::Sub:
            a -= b;
            break;
        case Op::Mul:
            a *= b;
            break;
        case Op::Shl:
            a <<= b & 31;
            break;
        case Op::Shr:
            a >>= b & 31;
            break;
        case Op::And:
            a &= b;
            break;
        case Op::Or:
            a |= b;
            break;
        case Op::Xor:
            a ^= b;
            break;
        case Op::UMin:
            a = std::min(a, b);
            break;
        case Op::UMax:
            a = std::max(a, b);
            break;
        default:
            break;
        }
    }
    return top ? stack[top - 1] : 0;
}

TablePlan::Mode TablePlan::GetMode() {
    static const Mode mode = [] {
        const char* env = std::getenv("BB_NATIVE_TABLES");
        const Mode chosen = !env || env[0] == '0' ? Mode::Walker
                            : env[0] == 'v' || env[0] == 'V' ? Mode::Verify
                                                             : Mode::Plan;
        std::printf("Native shaders: resource tables read by %s (BB_NATIVE_TABLES=0/1/verify)\n",
                    chosen == Mode::Walker ? "the walkers"
                    : chosen == Mode::Plan ? "table plans"
                                           : "table plans, the walkers compared");
        return chosen;
    }();
    return mode;
}

void TablePlan::CompareWithWalker(const void* walker, const u32* user_data, const u32* flat,
                                  u64 pgm_hash) const {
    static std::atomic<u32> reports{0}, fault_reports{0};
    thread_local std::vector<u32> by_walker;
    by_walker.assign(flat_dwords, 0);
    std::memcpy(by_walker.data(), flat, std::min(flat_dwords, FirstFlat) * sizeof(u32));
    // The walker's faults (a read of a table that is not mapped) skip the draw only when the
    // walker's values are used; here the plan's are.
    const u32 faults = Info::srt_walk_faults;
    reinterpret_cast<PFN_SrtWalker>(const_cast<void*>(walker))(user_data, by_walker.data());
    const u32 walker_faults = Info::srt_walk_faults - faults;
    Info::srt_walk_faults = faults;
    if (walker_faults && fault_reports.fetch_add(1, std::memory_order_relaxed) < 16) {
        std::printf("Native shaders: shader %016llx: the walker faulted %u times on its resource "
                    "tables (the walker path skips this draw); the plan read them without a "
                    "fault\n",
                    static_cast<unsigned long long>(pgm_hash), walker_faults);
    }
    u32 first = 0, count = 0;
    for (u32 d = FirstFlat; d < flat_dwords; ++d) {
        if (by_walker[d] != flat[d]) {
            first = count++ ? first : d;
        }
    }
    if (count && reports.fetch_add(1, std::memory_order_relaxed) < 32) {
        std::printf("Native shaders: shader %016llx: table plan and walker differ in %u of %u "
                    "words (first %u: walker %#x, plan %#x)\n",
                    static_cast<unsigned long long>(pgm_hash), count, flat_dwords - FirstFlat,
                    first, by_walker[first], flat[first]);
    }
}

} // namespace Shader::Native
