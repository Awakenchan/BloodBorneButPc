// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace BbNativeCalls {
using u32 = std::uint32_t;
using u64 = std::uint64_t;

enum class Mode { Off, Verify, Native };
Mode GetMode();

enum DrawKind : u32 { DrawNone = 0, DrawAuto = 1, DrawIndex = 2, DrawIndexOffset = 3 };

struct DrawRecord {
    u32 kind = DrawNone;
    u32 predicate = 0;
    u32 index_count = 0;
    u32 index_offset = 0;
    u64 index_addr = 0;
};

constexpr u32 ReservedDwords(u32 kind) {
    return kind == DrawAuto ? 5 : kind == DrawIndex ? 8 : kind == DrawIndexOffset ? 7 : 0;
}

inline u32 EncodeDrawPacket(const DrawRecord& r, u32* out) {
    const u32 pred = r.predicate & 1;
    switch (r.kind) {
    case DrawAuto:
        out[0] = 0xc0012d00u | pred;
        out[1] = r.index_count;
        out[2] = 2;
        return 3;
    case DrawIndex:
        out[0] = 0xc0042700u | pred;
        out[1] = r.index_count;
        out[2] = static_cast<u32>(r.index_addr) & ~1u;
        out[3] = static_cast<u32>(r.index_addr >> 32);
        out[4] = r.index_count;
        out[5] = 0;
        return 6;
    case DrawIndexOffset:
        out[0] = 0xc0033500u | pred;
        out[1] = r.index_count;
        out[2] = r.index_offset;
        out[3] = r.index_count;
        out[4] = 0;
        return 5;
    default:
        return 0;
    }
}

constexpr u32 TagMagic = 0xbb5d0000u;
constexpr u32 TagMask = 0xffff0000u;

inline u32 RecordCheck(const u32* payload, u32 count) {
    u32 h = 0x5eedbb01u;
    for (u32 k = 0; k < count; ++k) {
        h = (h ^ payload[k]) * 16777619u;
        h ^= h >> 15;
    }
    return h;
}

inline u32 EncodeRecordNop(const DrawRecord& r, u32* out) {
    const u32 n = ReservedDwords(r.kind);
    if (n == 0) {
        return 0;
    }
    const u32 payload = n - 1;
    out[0] = 0xc0001000u | ((n - 2) << 16);
    u32* p = out + 1;
    for (u32 k = 0; k < payload; ++k) {
        p[k] = 0;
    }
    p[0] = TagMagic | ((r.predicate & 1) << 8) | r.kind;
    p[1] = r.index_count;
    if (r.kind == DrawIndex) {
        p[2] = static_cast<u32>(r.index_addr);
        p[3] = static_cast<u32>(r.index_addr >> 32);
    } else if (r.kind == DrawIndexOffset) {
        p[2] = r.index_offset;
    }
    p[payload - 1] = RecordCheck(p, payload - 1);
    return n;
}

inline bool DecodeRecordNop(const u32* packet, DrawRecord& r) {
    const u32 header = packet[0];
    if ((header >> 30) != 3 || ((header >> 8) & 0xff) != 0x10) {
        return false;
    }
    const u32 count = (header >> 16) & 0x3fff;
    if (count < 3) {
        return false;
    }
    const u32* p = packet + 1;
    if ((p[0] & TagMask) != TagMagic || (p[0] & 0xfe00u) != 0) {
        return false;
    }
    const u32 kind = p[0] & 0xff;
    const u32 n = ReservedDwords(kind);
    if (n == 0 || count != n - 2) {
        return false;
    }
    const u32 payload = n - 1;
    if (p[payload - 1] != RecordCheck(p, payload - 1)) {
        return false;
    }
    r.kind = kind;
    r.predicate = (p[0] >> 8) & 1;
    r.index_count = p[1];
    r.index_offset = kind == DrawIndexOffset ? p[2] : 0;
    r.index_addr = kind == DrawIndex ? (u64(p[3]) << 32 | p[2]) : 0;
    return true;
}

inline bool RecordsEnabled() {
    static const bool on = GetMode() == Mode::Native;
    return on;
}
inline bool ReadRecord(const u32* packet, DrawRecord& r) {
    return RecordsEnabled() && DecodeRecordNop(packet, r);
}
inline bool IsRecordNop(const u32* packet) {
    DrawRecord r;
    return ReadRecord(packet, r);
}

void Remember(u64 address, const DrawRecord& record);
bool Take(u64 address, DrawRecord& record);

void NoteVerified(bool ok);
void NoteUnrecorded();
void NoteTraceTrailer();
void ReportMismatch(const char* what, u64 address, const DrawRecord& record, const u32* actual,
                    u32 words, u32 num_instances);

void NoteWritten(bool as_record);
void NoteDecoded();

bool TimingEnabled();
inline u64 Ticks() {
    return __builtin_ia32_rdtsc();
}
void AddPacketTicks(u64 ticks, bool is_draw);
void MaybeReport();
}
