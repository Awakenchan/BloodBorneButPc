// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: the native compiler's resource table plan (docs/native_shader_compiler.md, step 1).
// A shader reads its constants and resource descriptors (T#, V#, S#) through tables in the game's
// memory: a pointer in the user data, pointers in those tables, and so on. The inherited compiler
// generates x86 code per shader (the "SRT walker", flatten_extended_userdata_pass.cpp) that copies
// every value the shader reads from those tables into the flattened user data; a table pointer
// into unmapped memory faults there, a Windows exception per read. The plan is the same walk as
// data: one entry per flattened value (where its table pointer is, at which offset it sits), run
// by an interpreter whose every read of the game's memory is checked first. A table that cannot be
// read (null, or not mapped) reads as zeros, as its whole subtree does, and nothing faults. Plans
// are plain data: a shader catalog or pipeline cache can carry them, unlike machine code.
//
// Built from the program right after FlattenExtendedUserdataPass, which assigned each table read
// its place in the flattened user data. bb-shader-lab --tables compares every capture's plan with
// its walker; the game runs plans with BB_NATIVE_TABLES (GetMode), the walkers by default.
#pragma once

#include <cstring>
#include <string>
#include <vector>
#include "common/types.h"

namespace Serialization {
struct Archive;
}

namespace Shader::IR {
struct Program;
}

namespace Shader::Native {

struct TablePlan {
    /// The postfix operations of a dynamic offset (dwords, 32-bit arithmetic as the walker's).
    enum class Op : u8 {
        Imm,  ///< push value
        Flat, ///< push flat[value] (the user data are flat[0..16))
        Add,
        Sub,
        Mul,
        Shl, ///< shifts take the count's low 5 bits, as x86 does
        Shr,
        And,
        Or,
        Xor,
        Not,
        UMin,
        UMax,
        Bfe, ///< (base >> offset) & ((1 << count) - 1), offset and count & 31
    };
    struct ExprOp {
        Op op;
        u32 value;
    };
    static constexpr u32 MaxStack = 16;

    /// flat[FirstFlat + i] = the dword at table(parent) + 4 * offset. `parent` names where the
    /// table's pointer is: a user data register (below 16; the pointer is user data parent and
    /// parent + 1) or a flattened value (the pointer is the 64 bits at that value's own place).
    /// Pointers keep their low 48 bits, as the walker does.
    struct Entry {
        u16 parent;
        u16 expr_len; ///< 0: the offset is `offset`, else ops[offset, offset + expr_len)
        u32 offset;
    };
    static constexpr u32 FirstFlat = 16;
    /// Below this a table pointer is null: no guest memory is mapped there.
    static constexpr u64 NullBelow = 0x10000;

    std::vector<Entry> entries;
    std::vector<ExprOp> ops;
    /// Per flattened value that is a table's pointer (and per user data register): the dwords its
    /// constant offsets span, so a readable table is checked once, not per read. 0 elsewhere.
    std::vector<u32> table_dwords;
    u32 flat_dwords = FirstFlat;
    bool valid = false;
    std::string error; ///< why Build failed

    // For the census.
    u32 roots = 0;    ///< user data registers that hold table pointers
    u32 tables = 0;   ///< tables read, roots included
    u32 depth = 0;    ///< pointer levels below the user data (1: tables the user data point at)
    u32 dynamic = 0;  ///< entries with a computed offset

    /// The plan of `program`, after FlattenExtendedUserdataPass.
    void Build(const IR::Program& program);

    /// Reads the tables of `user_data` into flat[FirstFlat, flat_dwords), as the walker does, with
    /// every read of guest memory checked by `readable(address, bytes)` first; flat[0, FirstFlat)
    /// must hold the user data (Info::RefreshFlatBuf). Returns the number of reads refused.
    template <class Readable>
    u32 Run(const u32* user_data, u32* flat, Readable&& readable) const;

    /// An entry's offset in its table, in dwords, from the values flattened before it.
    u32 Offset(const Entry& entry, const u32* flat) const;

    /// The pipeline cache's shader record carries the plan after the walker (a record without one
    /// runs the walker). Defined with the other records, vk_pipeline_serialization.cpp.
    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);

    /// BB_NATIVE_TABLES: who reads the resource tables in the game. Walker (0, the default), Plan
    /// (1), or Verify: the plan's values are used, the walker runs beside it into a copy and every
    /// difference is logged. A shader without a valid plan (a record from an older engine) runs
    /// the walker in every mode.
    enum class Mode : u8 { Walker, Plan, Verify };
    static Mode GetMode();

    /// Verify: the walker over the same user data, compared with the plan's `flat`. Its faults
    /// are not counted as the draw's (the plan's values are used). Logs the first differences.
    /// `walker` is a PFN_SrtWalker.
    void CompareWithWalker(const void* walker, const u32* user_data, const u32* flat,
                           u64 pgm_hash) const;

    /// Whether [address, address + bytes) is mapped guest memory: set by the engine at start
    /// (shim/bbgpu.cpp). Plans do not run without it.
    static inline bool (*guest_readable)(u64 address, u64 bytes) = nullptr;

    /// bb-shader-lab: when set, TranslateProgram builds the plan of each program it translates
    /// there too (shader_recompiler/recompiler.cpp).
    static inline thread_local TablePlan* lab_target = nullptr;
};

template <class Readable>
u32 TablePlan::Run(const u32* user_data, u32* flat, Readable&& readable) const {
    if (!valid) {
        return 0;
    }
    // Per flattened value (or user data register) that is a table's pointer: the table's address
    // once read (0: null or unreadable), and whether its constant offsets were checked at once.
    // Pointers are read when the first entry below them needs them, after the value itself.
    static constexpr u64 Unread = ~u64{0};
    thread_local std::vector<u64> table, place;
    thread_local std::vector<u8> whole;
    table.assign(flat_dwords, Unread);
    place.assign(flat_dwords, 0); ///< where each flattened value was read
    whole.assign(flat_dwords, 0);
    u32 refused = 0;
    const auto pointer_of = [&](u16 parent) -> u64 {
        u64& ptr = table[parent];
        if (ptr != Unread) {
            return ptr;
        }
        u64 value = 0;
        if (parent < FirstFlat) {
            value = u64(user_data[parent]) |
                    (parent + 1 < FirstFlat ? u64(user_data[parent + 1]) << 32 : 0);
        } else if (const u64 at = place[parent]; at != 0) {
            if (readable(at, 8)) {
                std::memcpy(&value, reinterpret_cast<const void*>(at), 8);
            } else {
                ++refused;
            }
        }
        value &= 0xFFFFFFFFFFFFull;
        ptr = value < NullBelow ? 0 : value;
        if (ptr && table_dwords[parent] && readable(ptr, u64(table_dwords[parent]) * 4)) {
            whole[parent] = 1;
        }
        return ptr;
    };
    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& entry = entries[i];
        const u32 dst = FirstFlat + u32(i);
        const u64 base = pointer_of(entry.parent);
        if (base == 0) {
            flat[dst] = 0; // its table, or one above it, is null or could not be read
            continue;
        }
        const u64 at = base + u64(u32(Offset(entry, flat) << 2));
        u32 value = 0;
        if ((entry.expr_len == 0 && whole[entry.parent] &&
             entry.offset < table_dwords[entry.parent]) ||
            readable(at, 4)) {
            std::memcpy(&value, reinterpret_cast<const void*>(at), 4);
            place[dst] = at;
        } else {
            ++refused;
        }
        flat[dst] = value;
    }
    return refused;
}

} // namespace Shader::Native
