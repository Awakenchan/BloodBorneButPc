// SPDX-License-Identifier: GPL-2.0-or-later
#include "assets.h"

#include <windows.h>
#include <zlib.h>

#include <nlohmann/json.hpp>

#include <algorithm>

namespace start {
namespace {

const char* const kLayout = "dvdroot_ps4/map/mapstudio/m21_00_00_00.msb.dcx";
const char* const kInputSha = "8e1de0ee223cd5137a93383cfc7bb1d27964bc480c63e35013674717b403ef8f";
const char* const kOutputSha = "99d5c7b7f52307344c6e8296b499d5e57969f21c443fcc607cb42cb2c38c8afa";
const char* const kEbootSha[] = {"941f887a562aae054fac35af8cc8f27cf075f3d4cc2e029fb5ae2a663aaa5ae7",
                                 "cec1b276e7f9e4db978e57f524f41fbaac594530a3437b002e23f3fab14b4f86"};
const char* const kImageSha = "071df19c8880086d97182dbc057bc8cb37badaca57d9112683836b24a0444c0a";

std::uint32_t Be32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3];
}
void StoreBe32(std::uint8_t* p, std::uint32_t v) {
    p[0] = std::uint8_t(v >> 24), p[1] = std::uint8_t(v >> 16), p[2] = std::uint8_t(v >> 8), p[3] = std::uint8_t(v);
}

Bytes DcxUnpack(const Bytes& data) {
    if (data.size() < 4 || std::memcmp(data.data(), "DCX\0", 4)) return data;
    if (data.size() < 0x4c || std::memcmp(data.data() + 0x28, "DFLT", 4)) throw Error("Unsupported DCX format");
    const auto dca_it = std::search(data.begin(), data.end(), std::begin("DCA"), std::begin("DCA") + 4);
    const std::size_t dca = std::size_t(dca_it - data.begin());
    if (dca_it == data.end() || dca + 8 > data.size()) throw Error("DCX without a valid DCA header");
    const std::uint32_t skip = Be32(data.data() + dca + 4);
    if (skip < 8 || dca + skip >= data.size()) throw Error("Invalid DCX compressed offset");
    const std::uint32_t expected = Be32(data.data() + 0x1c);
    if (expected == 0 || expected > 32u * 1024 * 1024) throw Error("Invalid DCX uncompressed size");
    Bytes raw(expected + 1);
    z_stream z{};
    inflateInit(&z);
    z.next_in = const_cast<Bytef*>(data.data() + dca + skip);
    z.avail_in = uInt(data.size() - dca - skip);
    z.next_out = raw.data();
    z.avail_out = uInt(raw.size());
    const int status = inflate(&z, Z_FINISH);
    const std::size_t produced = raw.size() - z.avail_out;
    const bool tail = z.avail_in != 0;
    inflateEnd(&z);
    if (status != Z_STREAM_END || produced != expected || tail) throw Error("Invalid DCX compressed data or size");
    raw.resize(expected);
    return raw;
}

Bytes DcxPack(const Bytes& raw) {
    Bytes header = FromHex("44435800000100000000001800000024000000440000004c"
                           "4443530000000000000000004443500044464c5400000020"
                           "09000000000000000000000000000000000001014443410000000008");
    uLongf size = compressBound(uLong(raw.size()));
    Bytes packed(size);
    if (compress2(packed.data(), &size, raw.data(), uLong(raw.size()), 9) != Z_OK) throw Error("compression failed");
    packed.resize(size);
    StoreBe32(header.data() + 0x1c, std::uint32_t(raw.size()));
    StoreBe32(header.data() + 0x20, std::uint32_t(packed.size()));
    header.insert(header.end(), packed.begin(), packed.end());
    return header;
}

Bytes MirrorLayout(const Bytes& raw) {
    if (Sha256Hex(raw) != kInputSha) throw Error("Dream mirror requires the original 1.09 Dream layout");
    Bytes b = raw;
    const auto i32 = [&](std::size_t at) { return Load<std::int32_t>(b.data() + at); };
    std::vector<std::pair<std::size_t, std::vector<std::int64_t>>> lists;
    std::int64_t at = 0x10;
    for (int index = 0; index < 4; ++index) {
        if (at < 0 || std::size_t(at) > b.size() - 16) throw Error("Invalid MSB list header");
        const std::int32_t count = i32(std::size_t(at) + 4);
        if (count < 1 || std::size_t(count) > (b.size() - std::size_t(at) - 16) / 8) throw Error("Invalid MSB list size");
        std::vector<std::int64_t> offsets(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) offsets[std::size_t(i)] = Load<std::int64_t>(b.data() + std::size_t(at) + 16 + 8 * std::size_t(i));
        for (std::size_t i = 0; i + 1 < offsets.size(); ++i)
            if (offsets[i] <= 0 || offsets[i] >= std::int64_t(b.size())) throw Error("Invalid MSB entry");
        const std::int64_t next = offsets.back();
        offsets.pop_back();
        lists.emplace_back(std::size_t(at) + 16, offsets);
        at = next;
        if (index < 3 && (at <= 0 || at >= std::int64_t(b.size()))) throw Error("Invalid next MSB list");
    }
    const auto& events = lists[1].second;
    const std::size_t table = lists[3].first;
    const auto& parts = lists[3].second;
    constexpr int source = 807, target = 742, first_enemy = 736;
    if (parts.size() <= std::size_t(source + 1)) throw Error("Dream layout contains too few parts");
    std::vector<std::int64_t> starts;
    for (const auto& [_, entries] : lists) starts.insert(starts.end(), entries.begin(), entries.end());
    std::sort(starts.begin(), starts.end());
    const auto remap = [&](std::int32_t slot) {
        return slot == source ? target : (target <= slot && slot < source) ? slot + 1 : slot;
    };
    const std::pair<const std::vector<std::int64_t>*, std::vector<int>> groups[] = {
        {&events, {0x48, 0x50, 0x5c}}, {&parts, {0x128, 0x13c, 0x144, 0x178, 0x180, 0x188}}};
    for (const auto& [entries, fields] : groups) {
        for (const std::int64_t entry : *entries) {
            const auto next = std::upper_bound(starts.begin(), starts.end(), entry);
            const std::int64_t end = next == starts.end() ? std::int64_t(b.size()) : *next;
            for (const int field : fields) {
                if (entry + field + 4 > end) continue;
                const std::int32_t slot = i32(std::size_t(entry + field));
                if (slot >= 0 && remap(slot) != slot) Store<std::int32_t>(b.data() + entry + field, remap(slot));
            }
        }
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::int32_t kind = i32(std::size_t(parts[i]) + 0x14), number = i32(std::size_t(parts[i]) + 0x18);
        if (kind == 2 && int(i) >= target) Store<std::int32_t>(b.data() + parts[i] + 0x18, number + 1);
        if (kind == 10 && int(i) > source) Store<std::int32_t>(b.data() + parts[i] + 0x18, number - 1);
    }
    const std::int64_t moved = parts[source];
    if (i32(std::size_t(moved) + 0x14) != 10) throw Error("Mirror stand-in is not a DummyEnemy");
    Store<std::int32_t>(b.data() + moved + 0x14, 2);
    Store<std::int32_t>(b.data() + moved + 0x18, target - first_enemy);
    const std::int64_t start = parts[target], old = parts[source], end = parts[source + 1];
    if (!(start < old && old < end)) throw Error("Dream parts are not ordered");
    std::rotate(b.begin() + start, b.begin() + old, b.begin() + end);
    const std::int64_t size = end - old;
    for (int i = target; i < source; ++i) Store<std::int64_t>(b.data() + table + 8 * std::size_t(i + 1), parts[std::size_t(i)] + size);
    Store<std::int64_t>(b.data() + table + 8 * std::size_t(target), start);
    if (Sha256Hex(b) != kOutputSha) throw Error("Mirror layout does not match the verified result");
    return b;
}

// Ebrietas' trail effect (scripts/engine_assets.py EFFECT_FIXES, which says why): f001251720.fxr in
// Upper Cathedral Ward's and the chalice dungeons' effect binders becomes the Fishing Hamlet's
// f001454436.fxr. Hashes: the 1.09 .dcx, it unpacked, the result unpacked; and the entries.
struct EffectFix {
    const char* binder;
    const char* packed_sha;
    const char* input_sha;
    const char* output_sha;
};
const char* const kEffectSource = "dvdroot_ps4/sfx/frpg_sfxbnd_m36.ffxbnd.dcx";
const char* const kEffectSourceName = "f001454436.fxr";
const char* const kEffectSourceSha = "990644087f325191d34fd2f4b633a9f7c6a63ce9d71786c4289a7cae5cdfb4e0";
const char* const kEffectName = "f001251720.fxr";
const char* const kEffectSha = "988c55ba0f689bc0b4f8e7a9f9b184ef8cd9bcd4601b2c925387ff5ce3fc9f0b";
const EffectFix kEffectFixes[] = {
    {"dvdroot_ps4/sfx/frpg_sfxbnd_m24_02.ffxbnd.dcx", "6cab7031c5fafba95ed71c4ddb86e590b77b2d669988e8b53bed159e1dbc1015",
     "b0bd083145a753a667c4021a3f817879646b1f8b84201e2774daa17e1c5b0e99",
     "fae6b9639fb02b34188a3dd262b2695f7667812749f984f1eb2fd2ab298a9e9b"},
    {"dvdroot_ps4/sfx/frpg_sfxbnd_m29a.ffxbnd.dcx", "0466ebd7125cc705e341cab18e18c3d1282000a8d54b0628d47da992daed0fd6",
     "085f2d3d60d54e50fa981c7aad89ca42f47b3a733a78b40ee98f572883ce122b",
     "8348b2b7f110feb7ca0a444e8331886b85fe24ba422b8b9909cdc5d551ffcb07"},
};
constexpr std::size_t kBnd4Entry = 0x24; // flags, -1, packed and unpacked size, data offset (u32), id, name offset

struct BinderEntry {
    std::size_t header, offset, size;
    std::string name; // the file name, without its folders
};

/// The files of a BND4 with 0x24-byte entries (format 0x74: sizes, 32-bit offsets, ids, UTF-16
/// names), as the game's effect binders are.
std::vector<BinderEntry> BinderEntries(const Bytes& b) {
    if (b.size() < 0x40 || std::memcmp(b.data(), "BND4", 4) || Load<std::int64_t>(b.data() + 0x20) != std::int64_t(kBnd4Entry) ||
        b[0x30] != 1 || b[0x31] != 0x74)
        throw Error("Unexpected effect binder layout");
    const std::int32_t count = Load<std::int32_t>(b.data() + 0x0c);
    if (count < 0 || 0x40 + std::size_t(count) * kBnd4Entry > b.size()) throw Error("Unexpected effect binder layout");
    std::vector<BinderEntry> entries;
    for (std::int32_t i = 0; i < count; ++i) {
        const std::size_t at = 0x40 + std::size_t(i) * kBnd4Entry;
        const std::int64_t packed = Load<std::int64_t>(b.data() + at + 8), unpacked = Load<std::int64_t>(b.data() + at + 16);
        const std::uint32_t offset = Load<std::uint32_t>(b.data() + at + 24);
        const std::int32_t name_at = Load<std::int32_t>(b.data() + at + 32);
        if (packed != unpacked || packed < 0 || offset + std::uint64_t(packed) > b.size() || name_at < 0 ||
            std::size_t(name_at) >= b.size())
            throw Error("Unexpected effect binder entry");
        std::string name;
        for (std::size_t p = std::size_t(name_at); p + 1 < b.size() && (b[p] || b[p + 1]); p += 2) {
            const char c = b[p + 1] ? '?' : char(b[p]);
            name = c == '\\' || c == '/' ? std::string() : name + c;
        }
        entries.push_back({at, offset, std::size_t(packed), name});
    }
    return entries;
}

Bytes BinderFile(const Bytes& b, const std::string& name) {
    for (const BinderEntry& e : BinderEntries(b))
        if (e.name == name) return Bytes(b.begin() + std::ptrdiff_t(e.offset), b.begin() + std::ptrdiff_t(e.offset + e.size));
    throw Error(name + " is not in the effect binder");
}

/// The binder with `name` holding `data`: appended 16-byte aligned, its entry's sizes and offset
/// pointed at it; the other entries and the name table stay put.
Bytes ReplaceBinderFile(const Bytes& b, const std::string& name, const Bytes& data) {
    const auto entries = BinderEntries(b);
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const BinderEntry& e) { return e.name == name; });
    if (found == entries.end() ||
        std::count_if(entries.begin(), entries.end(), [&](const BinderEntry& e) { return e.name == name; }) != 1)
        throw Error(name + " is not in the effect binder once");
    const std::size_t offset = (b.size() + 15) & ~std::size_t(15);
    if (offset + data.size() > 0xffffffffu) throw Error("effect binder too large");
    Bytes out = b;
    out.resize(offset);
    out.insert(out.end(), data.begin(), data.end());
    Store<std::int64_t>(out.data() + found->header + 8, std::int64_t(data.size()));
    Store<std::int64_t>(out.data() + found->header + 16, std::int64_t(data.size()));
    Store<std::uint32_t>(out.data() + found->header + 24, std::uint32_t(offset));
    return out;
}

std::wstring Quote(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) return argument;
    std::wstring out = L"\"";
    for (std::size_t i = 0; i < argument.size(); ++i) {
        std::size_t slashes = 0;
        while (i < argument.size() && argument[i] == L'\\') ++slashes, ++i;
        if (i == argument.size()) {
            out.append(slashes * 2, L'\\');
            break;
        }
        if (argument[i] == L'"') out.append(slashes * 2 + 1, L'\\');
        else out.append(slashes, L'\\');
        out += argument[i];
    }
    return out + L"\"";
}

} // namespace

std::wstring CommandLine(const fs::path& program, const std::vector<std::wstring>& arguments) {
    std::wstring line = Quote(program.native());
    for (const std::wstring& a : arguments) line += L" " + Quote(a);
    return line;
}

int RunProgram(const fs::path& program, const std::vector<std::wstring>& arguments, std::string* output, void* log) {
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (output && !CreatePipe(&read, &write, &inherit, 0)) throw Error("cannot start " + Utf8(program));
    if (read) SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    HANDLE null = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW startup{sizeof startup};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write ? write : log ? static_cast<HANDLE>(log) : null;
    startup.hStdError = log ? static_cast<HANDLE>(log) : write ? write : null;
    std::wstring line = CommandLine(program, arguments);
    PROCESS_INFORMATION process{};
    const BOOL ok = CreateProcessW(program.c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                   &startup, &process);
    if (write) CloseHandle(write);
    if (!ok) {
        if (read) CloseHandle(read);
        CloseHandle(null);
        throw Error("cannot start " + Utf8(program));
    }
    if (read) {
        char buffer[4096];
        DWORD n = 0;
        while (::ReadFile(read, buffer, sizeof buffer, &n, nullptr) && n) output->append(buffer, n);
        CloseHandle(read);
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    CloseHandle(null);
    return int(code);
}

std::string VerifyExecutable(const fs::path& elf_path) {
    const Bytes elf = ReadFile(elf_path);
    const std::string sha = Sha256Hex(elf);
    // A patched 1.09 executable (prepare.cpp CheckGame: a few pieces changed, such as a 60 FPS
    // package, or CUSA00900's) passes too: every engine hook compares the code it replaces and
    // leaves itself out where the package changed it.
    std::string code;
    try {
        code = Trim(ReadText(elf_path.parent_path() / "game-code.txt"));
    } catch (const Error&) {
    }
    if (std::none_of(std::begin(kEbootSha), std::end(kEbootSha), [&](const char* s) { return sha == s; }) &&
        code != "1.09")
        throw Error("Mirror hooks require the verified 1.09 executable");
    if (elf.size() < 64) throw Error("Invalid executable program headers");
    const std::uint64_t phoff = Load<std::uint64_t>(elf.data() + 32);
    const std::uint16_t entsize = Load<std::uint16_t>(elf.data() + 54), count = Load<std::uint16_t>(elf.data() + 56);
    if (entsize != 56 || count == 0 || count >= 256) throw Error("Invalid executable program headers");
    const std::pair<std::uint64_t, const char*> hooks[] = {{0x1c1cce0, "554889e54157415653504d89ce4889fb"},
                                                           {0x2034770, "554889e54157415641554154534883ec38"}};
    for (const auto& [address, hex] : hooks) {
        const Bytes expected = FromHex(hex);
        bool found = false;
        for (int i = 0; i < count; ++i) {
            const std::uint8_t* p = elf.data() + phoff + std::uint64_t(i) * 56;
            if (phoff + std::uint64_t(i) * 56 + 56 > elf.size()) throw Error("Invalid executable program headers");
            const std::uint32_t kind = Load<std::uint32_t>(p), flags = Load<std::uint32_t>(p + 4);
            const std::uint64_t offset = Load<std::uint64_t>(p + 8), va = Load<std::uint64_t>(p + 16),
                                size = Load<std::uint64_t>(p + 32);
            if (kind == 1 && (flags & 1) && va <= address && address + expected.size() <= va + size) {
                const std::uint64_t at = offset + address - va;
                found = at + expected.size() <= elf.size() &&
                        std::equal(expected.begin(), expected.end(), elf.begin() + std::ptrdiff_t(at));
                break;
            }
        }
        if (!found) throw Error("Unexpected 1.09 engine bytes at " + Hex(address));
    }
    return kImageSha;
}

fs::path PrepareMirror(const fs::path& game, const fs::path& elf, const fs::path& cache) {
    VerifyExecutable(elf);
    const Bytes rewritten = MirrorLayout(DcxUnpack(ReadFile(game / Path(kLayout))));
    const fs::path destination = cache / "dream-mirror" / Path(kLayout);
    try {
        if (Sha256Hex(DcxUnpack(ReadFile(destination))) == kOutputSha) return fs::canonical(destination);
    } catch (const Error&) {
    }
    WriteFile(destination, DcxPack(rewritten));
    return fs::canonical(destination);
}

std::vector<std::pair<std::string, fs::path>> PrepareEffectFixes(const fs::path& game, const fs::path& cache,
                                                                 std::vector<std::string>& notes) {
    const fs::path root = cache / "effect-fixes";
    std::optional<Bytes> replacement;
    std::vector<std::pair<std::string, fs::path>> fixed;
    std::error_code error;
    for (const EffectFix& fix : kEffectFixes) {
        if (!fs::is_regular_file(game / Path(fix.binder), error)) continue; // not installed
        const Bytes packed = ReadFile(game / Path(fix.binder));
        const std::string file = Utf8(Path(fix.binder).filename());
        std::optional<Bytes> raw;
        if (Sha256Hex(packed) != fix.packed_sha) {
            raw = DcxUnpack(packed);
            if (Sha256Hex(*raw) != fix.input_sha) {
                notes.push_back(file + " is not the 1.09 file (a mod changes it): left as it is");
                continue;
            }
        }
        // A prepared binder is kept while it matches the hash written beside it (of its .dcx).
        const fs::path destination = root / Path(fix.binder);
        fs::path stamp = destination;
        stamp += L".sha256";
        try {
            if (Trim(ReadText(stamp)) == Sha256Hex(ReadFile(destination))) {
                fixed.emplace_back(fix.binder, fs::canonical(destination));
                continue;
            }
        } catch (const std::exception&) {
        }
        if (!raw) raw = DcxUnpack(packed);
        if (Sha256Hex(BinderFile(*raw, kEffectName)) != kEffectSha) throw Error(std::string("Unexpected ") + kEffectName + " in " + file);
        if (!replacement) {
            replacement = BinderFile(DcxUnpack(ReadFile(game / Path(kEffectSource))), kEffectSourceName);
            if (Sha256Hex(*replacement) != kEffectSourceSha) throw Error(std::string("Unexpected ") + kEffectSourceName);
        }
        const Bytes result = ReplaceBinderFile(*raw, kEffectName, *replacement);
        if (Sha256Hex(result) != fix.output_sha) throw Error(file + " does not match the verified result");
        const Bytes out = DcxPack(result);
        fs::create_directories(destination.parent_path());
        fs::path temporary = destination;
        temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
        WriteFile(temporary, out);
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            fs::remove(temporary, error);
            throw Error("cannot write " + Utf8(destination));
        }
        WriteText(stamp, Sha256Hex(out) + "\n");
        fixed.emplace_back(fix.binder, fs::canonical(destination));
    }
    return fixed;
}

fs::path PrepareMenuAssets(const fs::path& game, const fs::path& elf, const fs::path& cache, const fs::path& tool,
                           bool rebirth) {
    VerifyExecutable(elf);
    const fs::path root = cache / "pc-menus";
    std::vector<std::pair<std::string, std::string>> sources{{"dvdroot_ps4/menu/optionsetting.gfx", "movie"}};
    std::vector<std::string> messages;
    std::error_code error;
    const fs::path message_dir = game / "dvdroot_ps4" / "msg";
    if (fs::is_directory(message_dir, error))
        for (const auto& language : fs::directory_iterator(message_dir, error))
            if (fs::is_regular_file(language.path() / "menu.msgbnd.dcx", error))
                messages.push_back("dvdroot_ps4/msg/" + Utf8(language.path().filename()) + "/menu.msgbnd.dcx");
    std::sort(messages.begin(), messages.end());
    for (const std::string& m : messages) sources.emplace_back(m, "messages");
    if (sources.size() < 2 || sources.size() > 24)
        throw Error("Native options require valid menu bundles for every installed language");
    if (rebirth) sources.emplace_back("dvdroot_ps4/script/talk/m24_02_00_00.talkesdbnd.dcx", "rebirth");
    const std::string generator = Sha256Hex(ReadFile(tool));
    const fs::path manifest_path = root / "manifest.json";
    nlohmann::json previous = nlohmann::json::object();
    try {
        previous = nlohmann::json::parse(ReadText(manifest_path));
    } catch (const std::exception&) {
    }
    nlohmann::ordered_json rows = nlohmann::ordered_json::object();
    for (const auto& [relative, kind] : sources) {
        const fs::path source = game / Path(relative), destination = root / Path(relative);
        if (fs::file_size(source) > 64ull * 1024 * 1024) throw Error("Native menu source is oversized: " + relative);
        const std::string digest = Sha256Hex(ReadFile(source));
        std::optional<std::string> current;
        const auto prior = previous.contains("files") && previous["files"].contains(relative) ? previous["files"][relative]
                                                                                             : nlohmann::json::object();
        if (previous.value("generator", "") == generator && prior.value("input", "") == digest) {
            try {
                const std::string now = Sha256Hex(ReadFile(destination));
                if (now == prior.value("output", "")) current = now;
            } catch (const Error&) {
            }
        }
        if (!current) {
            fs::create_directories(destination.parent_path());
            fs::path temporary = destination;
            temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
            std::string errors;
            const int code = RunProgram(tool, {Wide(kind), source.native(), temporary.native()}, &errors);
            if (code) {
                fs::remove(temporary, error);
                const std::string message = Trim(errors);
                throw Error(message.empty() ? "Menu generator failed: " + relative : message);
            }
            current = Sha256Hex(ReadFile(temporary));
            if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                fs::remove(temporary, error);
                throw Error("cannot write " + Utf8(destination));
            }
        }
        rows[relative] = {{"input", digest}, {"output", *current}};
    }
    fs::create_directories(root);
    WriteText(manifest_path, nlohmann::ordered_json{{"generator", generator}, {"files", rows}}.dump(2));
    return fs::canonical(root);
}

} // namespace start
