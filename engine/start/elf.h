// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common.h"

#include <map>
#include <variant>

namespace start {

struct Ph {
    std::uint32_t type, flags;
    std::uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

struct Executable {
    Bytes elf;
    std::uint64_t entry = 0;
    std::vector<Ph> ph;
    std::vector<int> missing;
};

Executable ParseSelf(const Bytes& data);

using SfoValue = std::variant<std::uint32_t, std::string>;
std::map<std::string, SfoValue> ParseSfo(const Bytes& data);
std::string SfoText(const std::map<std::string, SfoValue>& sfo, const std::string& key, std::string_view fallback = "?");

std::vector<Ph> ProgramHeaders(const Bytes& elf);
std::optional<Bytes> OriginalBytes(const Bytes& elf, std::uint64_t offset, std::size_t length);

const std::uint8_t* Span(const Bytes& data, std::uint64_t offset, std::uint64_t size);

}
