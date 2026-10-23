// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace BbFreeCheck {
enum Source : std::uint32_t {
    Eop,
    Eos,
    EosGds,
    WriteData,
    ComputeWriteData,
    ReleaseMem,
    ConstRam,
    DmaFill,
    DmaCopy,
    BufferDownload,
    ImageDownload,
};
bool Enabled();
void Check(std::uint64_t address, std::uint64_t size, const void* data, Source source,
           std::uint64_t seq = 0);
std::uint64_t NextFenceSeq();
void NoteFenceDecoded(std::uint64_t label, std::uint64_t value, const void* packet,
                      const void* buffer, std::uint64_t submit);
void NoteFenceWriting(std::uint64_t label);
void NoteFenceWritten(std::uint64_t label, std::uint64_t write_ns = 0);
bool OnTrapFault(void* ucontext, std::uint64_t address);
bool OnStaleTrapFault(std::uint64_t address);
void NoteSubmit(std::uint64_t submit, const void* buffer, std::uint64_t size);
void DumpAtFault(std::uint64_t rax, std::uint64_t r14);
}
