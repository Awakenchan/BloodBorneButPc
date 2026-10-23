// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace BbWriteLog {
enum Source : std::uint32_t { Backing, WriteData, Fence, FenceIntent, WriteDataIntent, EosIntent, Dma };
bool Enabled();
void Note(std::uint64_t address, const void* data, std::uint64_t size, Source source);
void NoteIntent(std::uint64_t address, const void* data, std::uint64_t size, Source source);
void DumpRange(std::uint64_t address, std::uint64_t size);
}

extern "C" void bbgpu_dump_guest_writes(void* ucontext);
