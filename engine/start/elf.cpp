// SPDX-License-Identifier: GPL-3.0-or-later
#include "elf.h"

#include <algorithm>

namespace start {

const std::uint8_t* Span(const Bytes& data, std::uint64_t offset, std::uint64_t size) {
    if (offset > data.size() || size > data.size() - offset)
        throw Error("out of bounds: " + Hex(offset) + "+" + Hex(size) + " / " + Hex(data.size()));
    return data.data() + offset;
}

namespace {
constexpr std::uint64_t kLimit = 512ull << 20;

struct Header {
    std::uint64_t entry, phoff;
    std::uint16_t phentsize, phnum;
};

Header ReadHeader(const Bytes& data, std::uint64_t at) {
    const std::uint8_t* h = Span(data, at, 64);
    if (std::memcmp(h, "\x7f" "ELF\x02\x01\x01", 7) || Load<std::uint16_t>(h + 18) != 62)
        throw Error("expected little-endian x86-64 ELF");
    Header header{Load<std::uint64_t>(h + 24), Load<std::uint64_t>(h + 32), Load<std::uint16_t>(h + 54),
                  Load<std::uint16_t>(h + 56)};
    if (header.phentsize != 56 || header.phnum == 0 || header.phnum >= 256)
        throw Error("unsupported program headers");
    return header;
}

Ph ReadPh(const Bytes& data, std::uint64_t at) {
    const std::uint8_t* p = Span(data, at, 56);
    return Ph{Load<std::uint32_t>(p), Load<std::uint32_t>(p + 4), Load<std::uint64_t>(p + 8),
              Load<std::uint64_t>(p + 16), Load<std::uint64_t>(p + 24), Load<std::uint64_t>(p + 32),
              Load<std::uint64_t>(p + 40), Load<std::uint64_t>(p + 48)};
}

bool Required(std::uint32_t type) {
    return type == 1 || type == 2 || type == 0x61000000 || type == 0x61000010;
}

Executable ParseElf(const Bytes& data) {
    const Header header = ReadHeader(data, 0);
    Executable out;
    out.entry = header.entry;
    for (int i = 0; i < header.phnum; ++i) out.ph.push_back(ReadPh(data, header.phoff + std::uint64_t(i) * 56));
    std::uint64_t end = 0;
    for (const Ph& p : out.ph) end = std::max(end, p.offset + p.filesz);
    if (end > kLimit) throw Error("probe image exceeds 512 MiB limit");
    out.elf.assign(end, 0);
    std::copy_n(data.begin(), std::min<std::uint64_t>(end, data.size()), out.elf.begin());
    for (int i = 0; i < int(out.ph.size()); ++i) {
        const Ph& p = out.ph[i];
        if (p.filesz && p.offset + p.filesz > data.size()) {
            out.missing.push_back(i);
            if (Required(p.type)) throw Error("required segment " + std::to_string(i) + " is unavailable");
        }
    }
    return out;
}
}

Executable ParseSelf(const Bytes& data) {
    if (std::memcmp(Span(data, 0, 4), "\x7f" "ELF", 4) == 0) return ParseElf(data);
    if (std::memcmp(Span(data, 0, 4), "O\x15=\x1d", 4)) throw Error("expected a PS4 executable (SELF or ELF)");
    const std::uint16_t count = Load<std::uint16_t>(Span(data, 24, 2));
    const std::uint64_t base = 32 + std::uint64_t(count) * 32;
    const Header header = ReadHeader(data, base);
    Executable out;
    out.entry = header.entry;
    for (int i = 0; i < header.phnum; ++i)
        out.ph.push_back(ReadPh(data, base + header.phoff + std::uint64_t(i) * 56));
    std::uint64_t end = 0;
    for (const Ph& p : out.ph) end = std::max(end, p.offset + p.filesz);
    if (end > kLimit) throw Error("probe image exceeds 512 MiB limit");
    out.elf.assign(end, 0);
    const std::uint64_t header_end = header.phoff + std::uint64_t(header.phnum) * 56;
    const std::uint8_t* head = Span(data, base, header_end);
    if (header_end > out.elf.size()) out.elf.resize(header_end, 0);
    std::copy_n(head, header_end, out.elf.begin());
    std::vector<std::pair<std::uint64_t, std::uint64_t>> covered{{0, header_end}};
    for (int i = 0; i < count; ++i) {
        const std::uint8_t* s = Span(data, 32 + std::uint64_t(i) * 32, 32);
        const std::uint64_t flags = Load<std::uint64_t>(s), off = Load<std::uint64_t>(s + 8),
                            size = Load<std::uint64_t>(s + 16), mem = Load<std::uint64_t>(s + 24);
        if (!(flags & 0x800)) continue;
        if (flags & 10) throw Error("encrypted/compressed SELF segment is unsupported");
        const std::uint64_t index = (flags >> 20) & 4095;
        if (index >= out.ph.size()) throw Error("invalid segment index");
        const Ph& p = out.ph[index];
        if (size != p.filesz || size != mem) throw Error("unsupported blocked segment layout");
        const std::uint8_t* src = Span(data, off, size);
        if (p.offset + size > out.elf.size()) out.elf.resize(p.offset + size, 0);
        std::copy_n(src, size, out.elf.begin() + std::ptrdiff_t(p.offset));
        covered.emplace_back(p.offset, p.offset + size);
    }
    for (int i = 0; i < int(out.ph.size()); ++i) {
        const Ph& p = out.ph[i];
        if (!p.filesz) continue;
        const bool present = std::any_of(covered.begin(), covered.end(), [&](const auto& range) {
            return range.first <= p.offset && p.offset + p.filesz <= range.second;
        });
        if (!present) {
            out.missing.push_back(i);
            if (Required(p.type)) throw Error("required segment " + std::to_string(i) + " is unavailable");
        }
    }
    return out;
}

std::map<std::string, SfoValue> ParseSfo(const Bytes& data) {
    const std::uint8_t* h = Span(data, 0, 20);
    if (std::memcmp(h, "\0PSF", 4)) throw Error("bad SFO signature");
    const std::uint32_t keys = Load<std::uint32_t>(h + 8), values = Load<std::uint32_t>(h + 12),
                        count = Load<std::uint32_t>(h + 16);
    std::map<std::string, SfoValue> out;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* e = Span(data, 20 + std::uint64_t(i) * 16, 16);
        const std::uint16_t key = Load<std::uint16_t>(e), format = Load<std::uint16_t>(e + 2);
        const std::uint32_t size = Load<std::uint32_t>(e + 4), offset = Load<std::uint32_t>(e + 12);
        const std::uint64_t name_at = std::uint64_t(keys) + key;
        Span(data, name_at, 1);
        const auto name_end = std::find(data.begin() + std::ptrdiff_t(name_at), data.end(), 0);
        if (name_end == data.end()) throw Error("bad SFO key");
        const std::string name(data.begin() + std::ptrdiff_t(name_at), name_end);
        const std::uint8_t* v = Span(data, std::uint64_t(values) + offset, size);
        if (format == 0x404) {
            if (size < 4) throw Error("bad SFO integer");
            out[name] = Load<std::uint32_t>(v);
        } else {
            std::string text(reinterpret_cast<const char*>(v), size);
            while (!text.empty() && text.back() == '\0') text.pop_back();
            out[name] = text;
        }
    }
    return out;
}

std::string SfoText(const std::map<std::string, SfoValue>& sfo, const std::string& key, std::string_view fallback) {
    const auto it = sfo.find(key);
    if (it == sfo.end()) return std::string(fallback);
    if (const auto* text = std::get_if<std::string>(&it->second)) return *text;
    return std::to_string(std::get<std::uint32_t>(it->second));
}

std::vector<Ph> ProgramHeaders(const Bytes& elf) {
    const std::uint64_t phoff = Load<std::uint64_t>(Span(elf, 0x20, 8));
    const std::uint16_t entsize = Load<std::uint16_t>(Span(elf, 0x36, 2));
    const std::uint16_t count = Load<std::uint16_t>(Span(elf, 0x38, 2));
    std::vector<Ph> out;
    for (int i = 0; i < count; ++i) out.push_back(ReadPh(elf, phoff + std::uint64_t(i) * entsize));
    return out;
}

std::optional<Bytes> OriginalBytes(const Bytes& elf, std::uint64_t offset, std::size_t length) {
    for (const Ph& p : ProgramHeaders(elf)) {
        if (p.type == 1 && p.vaddr <= offset && offset + length <= p.vaddr + p.memsz) {
            const std::uint64_t start = offset - p.vaddr;
            Bytes data(length, 0);
            if (start < p.filesz) {
                const std::uint64_t n = std::min<std::uint64_t>(p.filesz - start, length);
                const std::uint64_t from = p.offset + start;
                const std::uint64_t avail = from < elf.size() ? std::min<std::uint64_t>(n, elf.size() - from) : 0;
                std::copy_n(elf.begin() + std::ptrdiff_t(from), avail, data.begin());
            }
            return data;
        }
    }
    return std::nullopt;
}

}
