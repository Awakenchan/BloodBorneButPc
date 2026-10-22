// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the shader lab. Compiles shader captures (BB_SHADER_CAPTURE=1, shader_recompiler/native/
// capture.h) offline, without the game or a GPU, and reports what each compile holds that is not
// native GPU code yet: goto variables left by the structurizer, EXEC read or written as data,
// ballots and lane operations, and bit casts. Later, the native compiler next to the current one.
//
//   bb-shader-lab <capture folder> [report.csv] [--native] [--spv-dir DIR]
//                                                 every capture, each in a child process
//   bb-shader-lab --one <file.bbs> [--native] [--spv-dir DIR]
//                                                 one capture, a CSV line on stdout
//   bb-shader-lab --export-catalog <cache folder> <catalog folder>
//                                                 a cache as a shader catalog: specializations,
//                                                 pipeline keys and draw states, without code
//   bb-shader-lab --seed-cache <catalog folder> <cache folder> <game folder> <eboot.elf>
//                                                 a catalog into a player's cache, the shader code
//                                                 from their own game files, then recompiled
//   bb-shader-lab --recompile-cache <cache folder>
//                                                 the cache recompiled with this compiler from
//                                                 its shader states (bb-start, after an update)
//   bb-shader-lab --walk-null <file.bbs>
//                                                 the capture's resource table walker run with
//                                                 null table pointers, then with tables of zeros:
//                                                 zeros out and no fault (a fault ends the process)
//   bb-shader-lab --census <capture folder> [report.csv]
//   bb-shader-lab --tables <capture folder> [report.csv]
//                                                 the native compiler's checks (see RunNative)
//   bb-shader-lab --bench <cache folder> [rounds] [report.csv] [--spv-dir DIR]
//                                                 the cache's states compiled in one process: time
//                                                 per phase, IR and SPIR-V size (see Bench)
// --native: the native compiler's options on (Profile::uniform_constant_buffers, dword layout).
// --spv-dir: each compile's SPIR-V written there (<capture>.spv) for spirv-val.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <new>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/wait.h>
#endif

#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/compile_stats.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/abstract_syntax_list.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/native/capture.h"
#include "shader_recompiler/native/table_plan.h"
#include "shader_recompiler/native/tone_curve.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/frontend/control_flow_graph.h"
#include "shader_recompiler/frontend/opcodes.h"
#include "shader_recompiler/ir/passes/srt.h"
#include "core/signals.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include <miniz.h>
#include <set>
#include "shader_recompiler/specialization.h"
#include "common/hash.h"
#include "common/serdes.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"

namespace fs = std::filesystem;
using namespace Shader;

// The pipeline serialization the cache recompile uses links in more of the GPU library than the
// shared test stubs (tests/test_gpu_runtime_stubs.cpp) cover; the lab never reaches these.
extern "C" {
void runtime_blocked_report(void) {}
[[noreturn]] void runtime_thread_exit(void*) { std::abort(); }
int runtime_pad_type(void) { return 0; }
int runtime_pad_button(uint32_t, int) { return 0; }
}

namespace {

bool g_native = false;
fs::path g_spv_dir;

std::vector<char> ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct Census {
    u32 insts = 0, goto_vars = 0, exec_reads = 0, exec_writes = 0, ballots = 0, lane_ops = 0,
        bitcasts = 0, discards = 0;
    u32 ifs = 0, loops = 0, breaks = 0;
};

Census Count(const IR::Program& program) {
    Census c;
    for (const IR::Block* block : program.blocks) {
        for (const IR::Inst& inst : block->Instructions()) {
            ++c.insts;
            switch (inst.GetOpcode()) {
            case IR::Opcode::GetGotoVariable:
            case IR::Opcode::SetGotoVariable:
                ++c.goto_vars;
                break;
            case IR::Opcode::GetExec:
                ++c.exec_reads;
                break;
            case IR::Opcode::SetExec:
                ++c.exec_writes;
                break;
            case IR::Opcode::Ballot:
            case IR::Opcode::InverseBallot:
            case IR::Opcode::BallotFindLsb:
                ++c.ballots;
                break;
            case IR::Opcode::ReadLane:
            case IR::Opcode::ReadFirstLane:
            case IR::Opcode::WriteLane:
            case IR::Opcode::Shuffle:
            case IR::Opcode::ShuffleXor:
            case IR::Opcode::QuadBroadcast:
                ++c.lane_ops;
                break;
            case IR::Opcode::BitCastU16F16:
            case IR::Opcode::BitCastU32F32:
            case IR::Opcode::BitCastF16U16:
            case IR::Opcode::BitCastF32U32:
                ++c.bitcasts;
                break;
            case IR::Opcode::Discard:
            case IR::Opcode::DiscardCond:
                ++c.discards;
                break;
            default:
                break;
            }
        }
    }
    for (const auto& node : program.syntax_list) {
        switch (node.type) {
        case IR::AbstractSyntaxNode::Type::If:
            ++c.ifs;
            break;
        case IR::AbstractSyntaxNode::Type::Loop:
            ++c.loops;
            break;
        case IR::AbstractSyntaxNode::Type::Break:
            ++c.breaks;
            break;
        default:
            break;
        }
    }
    return c;
}

/// Where a version 5 state's copy shader trailer starts (the size when it has none).
template <class Bytes>
size_t CopyTrailerAt(const Bytes& b) {
    if (b.size() < 8) return b.size();
    u32 count = 0, magic = 0;
    std::memcpy(&count, b.data() + b.size() - 8, 4);
    std::memcpy(&magic, b.data() + b.size() - 4, 4);
    if (magic != Native::CopyTrailerMagic || size_t(count) * 4 + 8 > b.size()) return b.size();
    return b.size() - 8 - size_t(count) * 4;
}

/// Dwords below 2^44 (a V#'s address range), reused by the next capture: the heap is above it.
u32* LowWords(size_t count) {
    static u32* block = nullptr;
    static size_t capacity = 0;
    if (count <= capacity) return block;
#ifdef _WIN32
    if (block) VirtualFree(block, 0, MEM_RELEASE);
    block = nullptr;
    capacity = 0;
    const size_t bytes = (count * 4 + 0xFFFF) & ~size_t(0xFFFF);
    for (uintptr_t at = 0x10000000; at < (uintptr_t(1) << 40) && !block; at += 0x10000000) {
        block = static_cast<u32*>(VirtualAlloc(reinterpret_cast<void*>(at), bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    }
    if (block) capacity = bytes / 4;
#endif
    return block;
}

/// A capture file read back for a compile: its buffers stay here, the user data's fetch shader and
/// V# table pointers aimed at them, and Info::lab_flat at the captured flattened user data.
struct LoadedCapture {
    Native::CaptureHeader header{};
    // RuntimeInfo has no default constructor (its unions): the captured bytes are the object.
    alignas(RuntimeInfo) unsigned char runtime_storage[sizeof(RuntimeInfo)]{};
    std::array<u32, ShaderParams::NumShaderUserData> user_data{};
    std::vector<u32> code, fetch, flat, copy;
    u32* tess = nullptr; ///< in low memory: a V#'s address has 44 bits
    std::map<u32, std::vector<u32>> tables;

    RuntimeInfo& Runtime() {
        return *std::launder(reinterpret_cast<RuntimeInfo*>(runtime_storage));
    }
    ShaderParams Params() {
        return ShaderParams{.user_data = user_data, .code = code, .hash = header.hash};
    }

    /// Null on success, else why the file is unusable.
    const char* Load(const fs::path& file) {
        const auto bytes = ReadAll(file);
        auto& h = header;
        if (bytes.size() < sizeof(h)) {
            return "capture cut short";
        }
        std::memcpy(&h, bytes.data(), sizeof(h));
        if (std::memcmp(h.magic, Native::CaptureMagic, 4) != 0 || h.version < 2 ||
            h.version > Native::CaptureVersion || h.runtime_info_size != sizeof(RuntimeInfo)) {
            return "capture of another engine version";
        }
        const char* p = bytes.data() + sizeof(h);
        const char* const end = bytes.data() + bytes.size();
        const auto take = [&](void* out, size_t size) {
            if (end < p + size) {
                return false;
            }
            std::memcpy(out, p, size);
            p += size;
            return true;
        };
        std::vector<u32> ud(h.user_data_count);
        code.resize(h.code_dwords);
        fetch.resize(h.fetch_dwords);
        // The flattened user data the resource table walker read in the game: the compile reads
        // sharps from it, and the walker would read the game's memory, which is not here.
        flat.resize(h.flat_dwords);
        if (!take(runtime_storage, sizeof(RuntimeInfo)) || !take(ud.data(), ud.size() * 4) ||
            !take(code.data(), code.size() * 4) || !take(fetch.data(), fetch.size() * 4) ||
            !take(flat.data(), flat.size() * 4)) {
            return "capture cut short";
        }
        std::memcpy(user_data.data(), ud.data(), std::min(ud.size(), user_data.size()) * 4);
        // The vertex attributes' V#s, read through table pointers in the user data: each table
        // is rebuilt here with the captured V#s at their offsets, and its pointer aimed at it.
        u32 sharp_count = 0;
        if (h.version >= 3 && !take(&sharp_count, 4)) {
            return "capture cut short";
        }
        std::vector<Native::CaptureSharp> sharps(sharp_count);
        if (!take(sharps.data(), sharps.size() * sizeof(Native::CaptureSharp))) {
            return "capture cut short";
        }
        for (const auto& sharp : sharps) {
            auto& table = tables[sharp.sgpr_base];
            if (table.size() < sharp.dword_offset + 4) {
                table.resize(sharp.dword_offset + 4);
            }
            std::memcpy(&table[sharp.dword_offset], sharp.words, sizeof(sharp.words));
        }
        // Version 4: the tessellation constants, their V#'s address aimed at the copy here.
        Native::CaptureTess tess_info{};
        if (h.version >= 4) {
            if (!take(&tess_info, sizeof(tess_info))) {
                return "capture cut short";
            }
            // bbport: a state without tessellation constants (size 0) needs no low memory.
            tess = tess_info.size ? LowWords((tess_info.size + 3) / 4) : nullptr;
            if (tess_info.size && !tess) {
                return "no low memory for the tessellation constants";
            }
            if (!take(tess, tess_info.size)) {
                return "capture cut short";
            }
        }
        if (tess_info.size) {
            AmdGpu::Buffer buffer;
            std::memcpy(&buffer, tess_info.words, sizeof(buffer));
            buffer.base_address = reinterpret_cast<uintptr_t>(tess);
            if (buffer.base_address != reinterpret_cast<uintptr_t>(tess)) {
                return "tessellation constants above the V# address range";
            }
            u32 words[4];
            std::memcpy(words, &buffer, sizeof(words));
            if (tess_info.sgpr_base == IR::NumScalarRegs) {
                if (tess_info.dword_offset + 4 <= user_data.size()) {
                    std::memcpy(&user_data[tess_info.dword_offset], words, sizeof(words));
                }
            } else {
                auto& table = tables[tess_info.sgpr_base];
                if (table.size() < tess_info.dword_offset + 4) {
                    table.resize(tess_info.dword_offset + 4);
                }
                std::memcpy(&table[tess_info.dword_offset], words, sizeof(words));
            }
        }
        for (auto& [sgpr, table] : tables) {
            if (sgpr + 1 < user_data.size()) {
                const u32* address = table.data();
                std::memcpy(&user_data[sgpr], &address, sizeof(address));
            }
        }
        if (!fetch.empty() && h.fetch_sgpr + 1 < user_data.size()) {
            // The fetch shader is read through its address in the user data: aim it at the copy.
            const u32* address = fetch.data();
            std::memcpy(&user_data[h.fetch_sgpr], &address, sizeof(address));
        }
        // Version 5: a geometry shader's copy shader, at the end of the file.
        if (const auto at = CopyTrailerAt(bytes); at != bytes.size()) {
            u32 count = 0;
            std::memcpy(&count, bytes.data() + bytes.size() - 8, 4);
            copy.resize(count);
            std::memcpy(copy.data(), bytes.data() + at, size_t(count) * 4);
        }
        if (HwStage(h.hw_stage) == HwStage::Geometry) {
            if (copy.empty()) {
                return "geometry shader without its copy shader (a state of an older engine)";
            }
            Runtime().hw.gs.vs_copy = copy;
        }
        Info::lab_flat = flat.data();
        Info::lab_flat_size = flat.size();
        return nullptr;
    }
};

/// --walk-null: the walker of one capture, with every table pointer in the user data null, then
/// aimed at a block of zeros (so the tables below are null). Both must fill the flattened values
/// with zeros without a fault: the PS4's GPU reads zeros at a null address, and a fault here was a
/// Windows exception per read in the game.
int WalkNull(const fs::path& file) {
    const auto profile_bytes = ReadAll(file.parent_path() / "profile.bin");
    Profile profile{};
    if (profile_bytes.size() >= 4) {
        std::memcpy(&profile, profile_bytes.data() + 4,
                    std::min<size_t>(profile_bytes.size() - 4, sizeof(Profile)));
    }
    LoadedCapture cap;
    if (const char* error = cap.Load(file)) {
        std::printf("%s: %s\n", file.filename().string().c_str(), error);
        return 2;
    }
    ShaderParams params = cap.Params();
    Info info(HwStage(cap.header.hw_stage), SwStage(cap.header.sw_stage), params);
    Pools pools;
    TranslateProgram(cap.code, pools, info, cap.Runtime(), profile);
    const auto walker = info.srt_info.walker_func;
    const u32 size = info.srt_info.flattened_bufsize_dw;
    if (!walker || size <= NUM_USER_DATA_REGS) {
        std::printf("%s: no walker\n", file.filename().string().c_str());
        return 0;
    }
    std::array<u32, 256> user_data{};
    std::vector<u32> flat(size);
    const auto zeros = [&] {
        for (u32 i = NUM_USER_DATA_REGS; i < size; ++i) {
            if (flat[i] != 0) {
                return false;
            }
        }
        return true;
    };
    // Null tables.
    std::fill(flat.begin(), flat.end(), 0xCDCDCDCDu);
    walker(user_data.data(), flat.data());
    if (!zeros()) {
        std::printf("%s: FAIL null tables: not all zeros\n", file.filename().string().c_str());
        return 1;
    }
    // Tables of zeros: every pointer pair in the user data aimed at 1 MiB of zeros.
    static std::vector<u32> block(1u << 18);
    const u64 address = reinterpret_cast<u64>(block.data());
    for (u32 i = 0; i + 1 < user_data.size(); i += 2) {
        user_data[i] = u32(address);
        user_data[i + 1] = u32(address >> 32);
    }
    std::fill(flat.begin(), flat.end(), 0xCDCDCDCDu);
    walker(user_data.data(), flat.data());
    if (!zeros()) {
        std::printf("%s: FAIL tables of zeros: not all zeros\n", file.filename().string().c_str());
        return 1;
    }
    std::printf("%s: PASS (%u flattened values)\n", file.filename().string().c_str(),
                size - NUM_USER_DATA_REGS);
    return 0;
}

/// BB_LAB_PROFILE="field=0,field=1": profile switches changed for an experiment (--bench, --one
/// and the folder run). False when a field is unknown.
bool ApplyLabProfile(Profile& profile) {
    const char* set = std::getenv("BB_LAB_PROFILE");
    if (!set) {
        return true;
    }
    const std::map<std::string, bool*> fields = {
        {"needs_manual_interpolation", &profile.needs_manual_interpolation},
        {"uniform_constant_buffers", &profile.uniform_constant_buffers},
        {"uniform_dword_layout", &profile.uniform_dword_layout},
        {"uniform_exact_base", &profile.uniform_exact_base},
        {"flatbuf_storage", &profile.flatbuf_storage},
        {"early_fragment_tests", &profile.early_fragment_tests},
        {"supports_fragment_shader_barycentric", &profile.supports_fragment_shader_barycentric},
        {"fragment_exact_fp", &profile.fragment_exact_fp},
        {"manual_interpolation_all", &profile.manual_interpolation_all},
    };
    const std::string list = set;
    for (size_t at = 0; at < list.size();) {
        const size_t end = std::min(list.find(',', at), list.size());
        const std::string item = list.substr(at, end - at);
        const size_t eq = item.find('=');
        const auto it = fields.find(item.substr(0, eq));
        if (eq == std::string::npos || it == fields.end()) {
            std::fprintf(stderr, "BB_LAB_PROFILE: unknown %s\n", item.c_str());
            return false;
        }
        *it->second = item[eq + 1] == '1';
        at = end + 1;
    }
    return true;
}

const char* kHeader = "file,stage,hash,gcn_dwords,status,ir_insts,spirv_words,goto_vars,exec_reads,"
                      "exec_writes,ballots,lane_ops,bitcasts,discards,ifs,loops,breaks";

/// One capture: compile it with the current recompiler, print a CSV line. Assertions end the
/// process (the parent reports it).
int RunOne(const fs::path& file, const fs::path& profile_file) {
    const auto profile_bytes = ReadAll(profile_file);
    Profile profile{};
    if (profile_bytes.size() < 4) {
        std::printf("%s,,,,no profile.bin\n", file.filename().string().c_str());
        return 2;
    }
    // A profile captured by an engine with fewer fields: the rest stay off.
    std::memcpy(&profile, profile_bytes.data() + 4,
                std::min<size_t>(profile_bytes.size() - 4, sizeof(Profile)));
    profile.uniform_constant_buffers = g_native;
    // bbport: the engine's default native layout (dwords, offsets in the push data).
    profile.uniform_dword_layout = g_native;
    profile.uniform_exact_base = false;
    if (!ApplyLabProfile(profile)) {
        return 2;
    }

    LoadedCapture cap;
    if (const char* error = cap.Load(file)) {
        std::printf("%s,,,,%s\n", file.filename().string().c_str(), error);
        return 2;
    }
    const auto& h = cap.header;
    const auto& code = cap.code;
    RuntimeInfo& runtime_info = cap.Runtime();
    ShaderParams params = cap.Params();
    Info info(HwStage(h.hw_stage), SwStage(h.sw_stage), params);
    Pools pools;
    const auto program = TranslateProgram(code, pools, info, runtime_info, profile);
    Backend::Bindings bindings{};
    const auto spv = Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, bindings);
    if (!g_spv_dir.empty()) {
        std::ofstream out(g_spv_dir / (file.stem().string() + ".spv"), std::ios::binary);
        out.write(reinterpret_cast<const char*>(spv.data()), std::streamsize(spv.size() * 4));
    }
    const Census c = Count(program);
    std::printf("%s,%u,%016llx,%zu,ok,%u,%zu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
                file.filename().string().c_str(), h.hw_stage, (unsigned long long)h.hash,
                code.size(), c.insts, spv.size(), c.goto_vars, c.exec_reads, c.exec_writes,
                c.ballots, c.lane_ops, c.bitcasts, c.discards, c.ifs, c.loops, c.breaks);
    // Every IR operation of the final program, for the run's histogram (ops.csv).
    std::map<std::string, u32> ops;
    for (const IR::Block* block : program.blocks) {
        for (const IR::Inst& inst : block->Instructions()) {
            ++ops[std::string(IR::NameOf(inst.GetOpcode()))];
        }
    }
    std::printf("OPS|");
    for (const auto& [name, n] : ops) {
        std::printf("%s=%u;", name.c_str(), n);
    }
    std::printf("\n");
    return 0;
}

#ifndef _WIN32
/// `text` as one word for /bin/sh.
std::string ShellQuote(const std::string& text) {
    std::string quoted = "'";
    for (const char c : text) {
        quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }
    return quoted + "'";
}

/// Runs this program with `args` (already quoted for /bin/sh) and returns its whole stdout and
/// exit code (-1: it did not exit by itself, e.g. the compiler crashed it).
std::string RunSelf(const fs::path& self, const std::string& args, int& exit_code) {
    std::string out;
    exit_code = -1;
    const std::string line = ShellQuote(fs::absolute(self).string()) + " " + args;
    if (FILE* pipe = popen(line.c_str(), "r")) {
        char buffer[4096];
        for (size_t got; (got = std::fread(buffer, 1, sizeof buffer, pipe)) > 0;) {
            out.append(buffer, got);
        }
        const int status = pclose(pipe);
        exit_code = status != -1 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return out;
}
#endif

#ifdef _WIN32
/// Runs this program with `args` and returns its whole stdout and exit code.
std::string RunSelf(const fs::path& self, const std::wstring& args, DWORD& exit_code) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    CreatePipe(&read, &write, &sa, 0);
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring line = L"\"" + self.wstring() + L"\" " + args;
    PROCESS_INFORMATION pi{};
    std::string out;
    exit_code = ~0u;
    if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &si, &pi)) {
        CloseHandle(write);
        write = nullptr;
        char buffer[4096];
        DWORD got = 0;
        while (ReadFile(read, buffer, sizeof buffer, &got, nullptr) && got) {
            out.append(buffer, got);
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &exit_code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    if (write) {
        CloseHandle(write);
    }
    CloseHandle(read);
    return out;
}

/// Runs `--one file` in a child process and returns its stdout (empty when it crashed).
std::string RunChild(const fs::path& self, const fs::path& file, DWORD& exit_code, std::string& ops) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    CreatePipe(&read, &write, &sa, 0);
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring line = L"\"" + self.wstring() + L"\" --one \"" + file.wstring() + L"\"";
    if (g_native) {
        line += L" --native";
    }
    if (!g_spv_dir.empty()) {
        line += L" --spv-dir \"" + g_spv_dir.wstring() + L"\"";
    }
    PROCESS_INFORMATION pi{};
    std::string out;
    exit_code = ~0u;
    if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &si, &pi)) {
        CloseHandle(write);
        write = nullptr;
        char buffer[4096];
        DWORD got = 0;
        while (ReadFile(read, buffer, sizeof buffer, &got, nullptr) && got) {
            out.append(buffer, got);
        }
        WaitForSingleObject(pi.hProcess, 60000);
        GetExitCodeProcess(pi.hProcess, &exit_code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    if (write) {
        CloseHandle(write);
    }
    CloseHandle(read);
    // Only the CSV line: the recompiler's own log lines go to stdout too.
    std::string csv;
    size_t at = 0;
    while (at < out.size()) {
        const size_t end = out.find('\n', at);
        std::string row = out.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (!row.empty() && row.back() == '\r') {
            row.pop_back();
        }
        if (row.rfind(file.filename().string(), 0) == 0) {
            csv = row;
        } else if (row.rfind("OPS|", 0) == 0) {
            ops = row.substr(4);
        }
        if (end == std::string::npos) {
            break;
        }
        at = end + 1;
    }
    return csv;
}
#endif

int RunAll(const fs::path& self, const fs::path& dir, const fs::path& report) {
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".bbs") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    std::ofstream csv(report);
    csv << kHeader << '\n';
    u32 ok = 0, crashed = 0;
    Census total;
    u32 with_goto = 0, with_exec = 0, with_ballot = 0, with_lane = 0;
    std::map<u32, u32> stages;
    std::map<std::string, std::map<u32, u64>> histogram; ///< IR operation -> hardware stage -> count
    for (size_t i = 0; i < files.size(); ++i) {
#ifdef _WIN32
        DWORD code = 0;
        std::string ops;
        std::string row = RunChild(self, files[i], code, ops);
#else
        std::string row, ops;
        u32 code = 1;
#endif
        if (row.empty() || code != 0) {
            ++crashed;
            csv << files[i].filename().string() << ",,,,crashed (exit " << code << ")\n";
            continue;
        }
        csv << row << '\n';
        ++ok;
        // stage, hash, dwords, status, insts, spirv, goto, exec_r, exec_w, ballots, lane, bitcasts
        u32 v[17] = {};
        unsigned long long hash = 0;
        char status[16] = {};
        size_t comma = row.find(',');
        std::sscanf(row.c_str() + comma + 1, "%u,%llx,%u,%15[^,],%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
                    &v[0], &hash, &v[1], status, &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8],
                    &v[9], &v[10], &v[11], &v[12], &v[13]);
        ++stages[v[0]];
        for (size_t at = 0; at < ops.size();) {
            const size_t end = ops.find(';', at);
            const std::string pair = ops.substr(at, end == std::string::npos ? std::string::npos : end - at);
            const size_t eq = pair.find('=');
            if (eq != std::string::npos) {
                const u64 n = std::strtoull(pair.c_str() + eq + 1, nullptr, 10);
                histogram[pair.substr(0, eq)][v[0]] += n;
            }
            if (end == std::string::npos) {
                break;
            }
            at = end + 1;
        }
        total.insts += v[2];
        total.goto_vars += v[4];
        total.exec_reads += v[5];
        total.exec_writes += v[6];
        total.ballots += v[7];
        total.lane_ops += v[8];
        total.bitcasts += v[9];
        with_goto += v[4] != 0;
        with_exec += (v[5] + v[6]) != 0;
        with_ballot += v[7] != 0;
        with_lane += v[8] != 0;
        if ((i + 1) % 200 == 0) {
            std::printf("%zu of %zu\n", i + 1, files.size());
        }
    }
    {
        // ops.csv beside the report: every IR operation, by hardware stage.
        std::ofstream out(fs::path(report).replace_filename(report.stem().string() + "-ops.csv"));
        out << "op,total,fragment,vertex,compute,other\n";
        for (const auto& [name, per_stage] : histogram) {
            unsigned long long total_n = 0, other = 0;
            for (const auto& [stage, n] : per_stage) {
                total_n += n;
                if (stage != 0 && stage != 1 && stage != 6) {
                    other += n;
                }
            }
            const auto at = [&](u32 st) {
                const auto it = per_stage.find(st);
                return it == per_stage.end() ? 0ull : (unsigned long long)it->second;
            };
            out << name << ',' << total_n << ',' << at(0) << ',' << at(1) << ',' << at(6) << ',' << other
                << '\n';
        }
    }
    std::printf("Shader lab: %zu captures, %u compiled, %u crashed the current recompiler\n",
                files.size(), ok, crashed);
    for (const auto& [stage, n] : stages) {
        std::printf("  hardware stage %u: %u\n", stage, n);
    }
    std::printf("  with goto variables: %u (%u in all)\n", with_goto, total.goto_vars);
    std::printf("  with EXEC read or written as data: %u (%u reads, %u writes)\n", with_exec,
                total.exec_reads, total.exec_writes);
    std::printf("  with ballots: %u (%u)\n", with_ballot, total.ballots);
    std::printf("  with lane operations: %u (%u)\n", with_lane, total.lane_ops);
    std::printf("  bit casts: %u in %u IR instructions\n", total.bitcasts, total.insts);
    std::printf("Per shader: %s\n", report.string().c_str());
    return 0;
}

// --- The cache recompile (bb-start runs it when the shader compiler changed) ---------------------
// The cache folder holds per permutation "<pgm>_<perm>.spv" (SPIR-V), "<perm_hash>.meta" (its
// specialization and Info) and, since 0.9.34, "<pgm>_<perm>.bbs" (its compile inputs, the shader
// state). Each permutation with a state is compiled again; it is kept when this compiler's
// specialization equals the stored one (the pipeline keys name permutations by it), and left out
// otherwise. The pipeline keys stay: the engine's warm-up builds every pipeline whose shaders are
// there before the game starts, instead of each one compiling in play.

std::vector<std::string> SpvNames(const fs::path& dir);

/// The permutations to compile: those listed in `only` (one name a line) when given, else all.
std::vector<std::string> BatchNames(const fs::path& dir, const fs::path& only) {
    if (only.empty()) {
        return SpvNames(dir);
    }
    std::vector<std::string> names;
    std::ifstream in(only);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) names.push_back(line);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> SpvNames(const fs::path& dir) {
    // The permutations with SPIR-V, and those with only a state (seeded from a catalog).
    std::set<std::string> names;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(dir, error)) {
        const auto ext = entry.path().extension();
        if (ext == ".spv" || ext == Native::StateExtension) {
            names.insert(entry.path().stem().string());
        }
    }
    return {names.begin(), names.end()};
}

bool WriteAtomically(const fs::path& path, const void* data, size_t size) {
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char*>(data), std::streamsize(size));
        if (!out) {
            return false;
        }
    }
    std::error_code error;
    fs::rename(temporary, path, error);
    return !error;
}

fs::path MetaPath(const fs::path& dir, u64 pgm, size_t perm) {
    char name[40];
    std::snprintf(name, sizeof(name), "0x%016llx.meta", (unsigned long long)HashCombine(pgm, u64(perm)));
    return dir / name;
}

/// The permutations (HashCombine(pgm, perm), 0 for none) a pipeline key's stages name: the
/// record's version and kind, then the key, whose first field is the stage hashes (one for
/// compute).
std::vector<u64> KeyStageHashes(const std::vector<char>& key) {
    u32 version = 0, is_compute = 0;
    if (key.size() < 8) return {};
    std::memcpy(&version, key.data(), 4);
    std::memcpy(&is_compute, key.data() + 4, 4);
    const size_t count = is_compute ? 1 : size_t(SwStage::NumLogicalStages);
    if (is_compute > 1 || key.size() < 8 + count * 8) return {};
    std::vector<u64> hashes(count);
    std::memcpy(hashes.data(), key.data() + 8, count * 8);
    return hashes;
}

/// bbport (docs/pipeline_compiles.md): pipeline keys whose stages lost their specialization (left
/// out by a recompile, or by an earlier engine) are removed. They could not be built at the start
/// anyway, and a later permutation of the same program takes the free number again: the key would
/// then join that permutation to the old one's vertex inputs.
u32 RemoveOrphanKeys(const fs::path& dir) {
    u32 removed = 0;
    std::error_code error;
    std::vector<fs::path> orphans;
    for (const auto& entry : fs::directory_iterator(dir, error)) {
        if (entry.path().extension() != ".key") continue;
        for (const u64 hash : KeyStageHashes(ReadAll(entry.path()))) {
            char name[40];
            std::snprintf(name, sizeof(name), "0x%016llx.meta", (unsigned long long)hash);
            if (hash && !fs::exists(dir / name, error)) {
                orphans.push_back(entry.path());
                break;
            }
        }
    }
    for (const auto& path : orphans) removed += fs::remove(path, error) ? 1 : 0;
    return removed;
}

void Leave(const fs::path& dir, const std::string& name, u64 pgm, size_t perm) {
    std::error_code error;
    fs::remove(dir / (name + ".spv"), error);
    // Its state too: the permutation compiles again in play and keeps a fresh one.
    fs::remove(dir / (name + Native::StateExtension), error);
    fs::remove(MetaPath(dir, pgm, perm), error);
}

/// Child: the permutations from `start` on, one line each ("@OK i", "@OUT i why"), then "@END".
/// It only reports what to leave out: the parent removes those files once every batch is done, so
/// each batch lists the same names in the same order.
int RecompileBatch(const fs::path& dir, size_t start, const fs::path& only) {
    const auto profile_bytes = ReadAll(dir / "profile.bin");
    Profile profile{};
    if (profile_bytes.size() != sizeof(Profile)) {
        std::printf("\n@PROFILE\n"); // a profile of another engine: the cache is started again
        return 3;
    }
    std::memcpy(&profile, profile_bytes.data(), sizeof(Profile));
    const auto names = BatchNames(dir, only);
    Pools pools;
    for (size_t i = start; i < names.size(); ++i) {
        const auto& name = names[i];
        unsigned long long pgm = 0;
        size_t perm = 0;
        const auto out = [&](const char* why) {
            std::printf("\n@OUT %zu %s\n", i, why);
            std::fflush(stdout);
        };
        if (std::sscanf(name.c_str(), "0x%llx_%zu", &pgm, &perm) != 2) {
            out("name");
            continue;
        }
        const auto meta_bytes = ReadAll(MetaPath(dir, pgm, perm));
        StageSpecialization old_spec;
        size_t meta_perm = ~size_t{};
        if (meta_bytes.empty() ||
            !Vulkan::ReadShaderMetaSpec(std::vector<u8>(meta_bytes.begin(), meta_bytes.end()),
                                        old_spec, meta_perm) ||
            meta_perm != perm) {
            out("record of another engine");
            continue;
        }
        LoadedCapture cap;
        if (cap.Load(dir / (name + Native::StateExtension))) {
            out("no state");
            continue;
        }
        // Which permutation is being compiled, should the compiler end the process here.
        std::printf("\n@AT %zu %u %u\n", i, cap.header.hw_stage, cap.header.sw_stage);
        std::fflush(stdout);
        // A catalog's state has no fetch shader code (the game generates it at run time): its
        // layout comes from the specialization instead.
        Gcn::fetch_override = cap.fetch.empty() && cap.header.fetch_sgpr != ~0u && old_spec.fetch_shader_data
                                  ? &*old_spec.fetch_shader_data
                                  : nullptr;
        ShaderParams params = cap.Params();
        Info info(HwStage(cap.header.hw_stage), SwStage(cap.header.sw_stage), params);
        // The RuntimeInfo before the compile, as the engine's CompileModule has it.
        RuntimeInfo runtime_info = cap.Runtime();
        const auto program = TranslateProgram(cap.code, pools, info, runtime_info, profile);
        Backend::Bindings bindings = old_spec.start;
        auto spv = Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, bindings);
        const StageSpecialization new_spec(info, runtime_info, profile, old_spec.start);
        old_spec.info = &info;
        if (!(new_spec == old_spec)) {
            // What differs, for the report (BB_LAB_VERBOSE=1 lists them).
            std::string why = "specialization changed:";
            if (!(new_spec.runtime_info == old_spec.runtime_info)) why += " runtime";
            if (new_spec.vs_attribs != old_spec.vs_attribs) why += " vs_attribs";
            if (new_spec.fetch_shader_data != old_spec.fetch_shader_data) why += " fetch";
            if (new_spec.fmasks != old_spec.fmasks) why += " fmasks";
            if (new_spec.start != old_spec.start) why += " start";
            if (new_spec.buffers.size() != old_spec.buffers.size() || new_spec.buffers != old_spec.buffers) why += " buffers";
            if (new_spec.images.size() != old_spec.images.size() || new_spec.images != old_spec.images) why += " images";
            if (new_spec.samplers.size() != old_spec.samplers.size() || new_spec.samplers != old_spec.samplers) why += " samplers";
            if (new_spec.bitset != old_spec.bitset) why += " bitset";
            out(why.c_str());
            continue;
        }
        const auto meta = Vulkan::SerializeShaderMeta(info, new_spec, HashCombine(pgm, u64(perm)), perm);
        if (!WriteAtomically(MetaPath(dir, pgm, perm), meta.data(), meta.size()) ||
            !WriteAtomically(dir / (name + ".spv"), spv.data(), spv.size() * sizeof(u32))) {
            out("write failed");
            continue;
        }
        std::printf("\n@OK %zu\n", i);
        std::fflush(stdout);
    }
    std::printf("\n@END\n");
    return 0;
}

// --- Keys of a stage compiled beside another fragment shader (bb-shader-lab --fix-keys <cache>) --
// bbport (docs/pipeline_compiles.md, 0.9.37 beta 9): engines up to 0.9.37 beta 6 reused a module
// compiled after one fragment shader beside another, whose bindings end elsewhere. The warm-up leaves
// such keys out (LoadGraphicsPipeline: each stage's bindings start where the stages before it end),
// so the pipeline compiled in play on every PC a catalog with them seeded (150 of the catalog's
// 1085 keys). Here each such stage is compiled again from its state with the start the key's
// earlier stages leave, or a permutation of its program that has that start is used, and the key
// names it: the warm-up then builds it. A child process (RecompileCache): a stage the compiler
// refuses ends only this.

std::set<size_t> UsedPerms(const fs::path& cache, u64 pgm);

/// One permutation's record (its .meta), as the warm-up reads it.
struct StageRecord {
    u64 pgm = 0;
    size_t perm = 0;
    std::unique_ptr<Info> info;
    StageSpecialization spec{};
    std::optional<Gcn::FetchShaderData> fetch;
};

int FixKeys(const fs::path& dir) {
    const auto profile_bytes = ReadAll(dir / "profile.bin");
    Profile profile{};
    if (profile_bytes.size() != sizeof(Profile)) {
        std::printf("\n@FIXED 0 0 0\n");
        return 3;
    }
    std::memcpy(&profile, profile_bytes.data(), sizeof(Profile));
    std::error_code error;
    std::map<u64, std::unique_ptr<StageRecord>> records; // by permutation hash; null: unreadable
    const auto record = [&](u64 hash) -> StageRecord* {
        auto& slot = records[hash];
        if (slot) return slot.get();
        char name[40];
        std::snprintf(name, sizeof(name), "0x%016llx.meta", (unsigned long long)hash);
        const auto meta = ReadAll(dir / name);
        if (meta.empty()) return nullptr;
        auto r = std::make_unique<StageRecord>();
        r->info = std::make_unique<Info>();
        r->spec.info = r->info.get();
        Serialization::Archive ar{std::vector<u8>(meta.begin(), meta.end())};
        try {
            if (!Vulkan::LoadShaderMeta(ar, *r->info, r->fetch, r->spec, r->perm, hash)) return nullptr;
        } catch (const std::exception&) {
            return nullptr;
        }
        r->pgm = r->info->pgm_hash;
        slot = std::move(r);
        return slot.get();
    };
    // `from` with its bindings starting at `start`: a permutation of its program that has that
    // specialization, else one compiled now from `from`'s state. 0 when neither is possible.
    std::map<std::tuple<u64, size_t, u32, u32>, u64> made; // (pgm, perm, unified, buffer) -> hash
    Pools pools;
    u32 compiled = 0;
    const auto with_start = [&](StageRecord& from, Backend::Bindings start) -> u64 {
        const auto made_key = std::make_tuple(from.pgm, from.perm, start.unified, start.buffer);
        if (const auto it = made.find(made_key); it != made.end()) return it->second;
        start.user_data = from.spec.start.user_data;
        StageSpecialization wanted = from.spec;
        wanted.start = start;
        const auto used = UsedPerms(dir, from.pgm);
        // The start compared as the warm-up does: the specialization's own comparison leaves it out
        // for a stage without guest resources.
        for (const size_t perm : used) {
            StageRecord* other = record(HashCombine(from.pgm, u64(perm)));
            if (other && other->pgm == from.pgm && other->spec == wanted &&
                other->spec.start.unified == start.unified && other->spec.start.buffer == start.buffer) {
                return made[made_key] = HashCombine(from.pgm, u64(perm));
            }
        }
        char state_name[48];
        std::snprintf(state_name, sizeof(state_name), "0x%016llx_%zu", (unsigned long long)from.pgm, from.perm);
        const fs::path state_path = dir / (std::string(state_name) + Native::StateExtension);
        LoadedCapture cap;
        if (cap.Load(state_path)) return made[made_key] = 0;
        Gcn::fetch_override = cap.fetch.empty() && cap.header.fetch_sgpr != ~0u && from.spec.fetch_shader_data
                                  ? &*from.spec.fetch_shader_data
                                  : nullptr;
        ShaderParams params = cap.Params();
        Info info(HwStage(cap.header.hw_stage), SwStage(cap.header.sw_stage), params);
        RuntimeInfo runtime_info = cap.Runtime();
        const auto program = TranslateProgram(cap.code, pools, info, runtime_info, profile);
        Backend::Bindings bindings = start;
        const auto spv = Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, bindings);
        StageSpecialization spec(info, runtime_info, profile, start);
        if (!(spec == wanted)) return made[made_key] = 0; // the state compiles otherwise now
        size_t perm = used.empty() ? 0 : *used.rbegin() + 1;
        if (perm >= 4096) return made[made_key] = 0;
        const u64 hash = HashCombine(from.pgm, u64(perm));
        char name[48];
        std::snprintf(name, sizeof(name), "0x%016llx_%zu", (unsigned long long)from.pgm, perm);
        const auto meta = Vulkan::SerializeShaderMeta(info, spec, hash, perm);
        const auto state = ReadAll(state_path);
        if (!WriteAtomically(dir / (std::string(name) + Native::StateExtension), state.data(), state.size()) ||
            !WriteAtomically(dir / (std::string(name) + ".spv"), spv.data(), spv.size() * sizeof(u32)) ||
            !WriteAtomically(MetaPath(dir, from.pgm, perm), meta.data(), meta.size())) {
            return made[made_key] = 0;
        }
        ++compiled;
        return made[made_key] = hash;
    };

    std::vector<fs::path> keys;
    for (const auto& entry : fs::directory_iterator(dir, error)) {
        if (entry.path().extension() == ".key") keys.push_back(entry.path());
    }
    std::sort(keys.begin(), keys.end());
    u32 fixed = 0, failed = 0;
    for (const auto& path : keys) {
        const auto bytes = ReadAll(path);
        u32 version = 0, is_compute = 1;
        if (bytes.size() >= 8) {
            std::memcpy(&version, bytes.data(), 4);
            std::memcpy(&is_compute, bytes.data() + 4, 4);
        }
        if (!(version == 5 || version == 6) || is_compute != 0) continue;
        auto hashes = KeyStageHashes(bytes);
        Backend::Bindings expected{};
        bool changed = false, ok = true;
        for (size_t stage = 0; stage < hashes.size() && stage < 5 && ok; ++stage) {
            if (!hashes[stage]) continue;
            StageRecord* r = record(hashes[stage]);
            if (!r) {
                ok = false;
                break;
            }
            for (const auto& image : r->info->images) {
                ok = ok && image.mip_fallback_mode != MipStorageFallbackMode::DynamicIndex;
            }
            if (!ok) break;
            if (r->spec.start.unified != expected.unified || r->spec.start.buffer != expected.buffer) {
                const u64 target = with_start(*r, expected);
                if (!target) {
                    ok = false;
                    break;
                }
                hashes[stage] = target;
                changed = true;
            }
            expected.buffer += u32(r->info->buffers.size());
            expected.unified += u32(r->info->buffers.size() + r->info->images.size() + r->info->samplers.size());
        }
        if (!changed) continue;
        if (!ok) {
            ++failed;
            continue;
        }
        // The key again with the new stages, under the name the engine gives it (g_ and its hash);
        // the pipeline's state after the key stays as it was. The file of that name is written over:
        // the engine's own save of the same pipeline is the same key, and a seeded key keeps its
        // catalog name while its stages are renumbered (SeedCache), so that name may hold a key
        // with old stages.
        Serialization::Archive ar{std::vector<u8>(bytes.begin() + 8, bytes.end())};
        Vulkan::GraphicsPipelineKey key;
        if (!key.Deserialize(ar)) {
            ++failed;
            continue;
        }
        const std::vector<u8> rest(ar.CurrPtr(), ar.CurrPtr() + ar.Remaining());
        Serialization::Archive check;
        key.Serialize(check);
        if (check.TakeOff().size() + rest.size() + 8 != bytes.size()) {
            ++failed; // the key does not read back as it was written: left as it is
            continue;
        }
        for (size_t stage = 0; stage < hashes.size() && stage < key.stage_hashes.size(); ++stage) {
            key.stage_hashes[stage] = hashes[stage];
        }
        Serialization::Archive out;
        key.Serialize(out);
        std::vector<u8> written(bytes.begin(), bytes.begin() + 8);
        const auto key_bytes = out.TakeOff();
        written.insert(written.end(), key_bytes.begin(), key_bytes.end());
        written.insert(written.end(), rest.begin(), rest.end());
        char name[48];
        std::snprintf(name, sizeof(name), "g_0x%016llx.key",
                      (unsigned long long)std::hash<Vulkan::GraphicsPipelineKey>{}(key));
        const fs::path target = dir / name;
        if (!WriteAtomically(target, written.data(), written.size())) {
            ++failed;
            continue;
        }
        if (target != path) fs::remove(path, error);
        ++fixed;
    }
    std::printf("\n@FIXED %u %u %u\n", fixed, compiled, failed);
    return 0;
}

/// Parent: batches in child processes; a permutation that crashes the compiler is left out and
/// the next batch starts after it.
int RecompileCache(const fs::path& self, const fs::path& dir, const fs::path& only = {}) {
    const auto names = BatchNames(dir, only);
    size_t next = 0, kept = 0, left = 0, crashed = 0;
    std::vector<size_t> leave;
    const char* verbose_env = std::getenv("BB_LAB_VERBOSE");
    const bool verbose = verbose_env && verbose_env[0] == '1';
    std::map<std::string, u32> reasons; // why permutations were left out, for BB_LAB_VERBOSE=1
    while (next < names.size()) {
#ifdef _WIN32
        DWORD code = 0;
        const std::string output =
            RunSelf(self, L"--recompile-batch \"" + dir.wstring() + L"\" " + std::to_wstring(next) +
                              (only.empty() ? std::wstring{} : L" \"" + only.wstring() + L"\""),
                    code);
#else
        int code = 0;
        const std::string output =
            RunSelf(self, "--recompile-batch " + ShellQuote(dir.string()) + " " + std::to_string(next) +
                              (only.empty() ? std::string{} : " " + ShellQuote(only.string())),
                    code);
#endif
        if (output.find("@PROFILE") != std::string::npos) {
            std::printf("Shader cache: written by an engine with another profile; started again\n");
            return 3;
        }
        long long last = -1;
        std::string at_stage;
        bool ended = false;
        size_t at = 0;
        while (at < output.size()) {
            const size_t end = output.find('\n', at);
            const std::string row = output.substr(at, end == std::string::npos ? std::string::npos : end - at);
            size_t index = 0;
            if (std::sscanf(row.c_str(), "@OK %zu", &index) == 1) {
                ++kept;
                last = (long long)index;
            } else if (std::sscanf(row.c_str(), "@OUT %zu", &index) == 1) {
                ++left;
                leave.push_back(index);
                last = (long long)index;
                const auto why = row.find(' ', 5);
                ++reasons[why == std::string::npos ? "?" : row.substr(why + 1)];
            } else if (row.rfind("@AT ", 0) == 0) {
                at_stage = row.substr(4);
            } else if (row.rfind("@END", 0) == 0) {
                ended = true;
            }
            if (end == std::string::npos) {
                break;
            }
            at = end + 1;
        }
        if (ended) {
            break;
        }
        // The child stopped at the permutation after its last line: that one is left out.
        const size_t failed = last < 0 ? next : size_t(last) + 1;
        if (failed >= names.size()) {
            break;
        }
        leave.push_back(failed);
        ++crashed;
        ++reasons["compiler refused, hw/sw stage " + at_stage.substr(at_stage.find(' ') + 1)];
        next = failed + 1;
        (void)code;
    }
    for (const size_t index : leave) {
        unsigned long long pgm = 0;
        size_t perm = 0;
        std::sscanf(names[index].c_str(), "0x%llx_%zu", &pgm, &perm);
        Leave(dir, names[index], pgm, perm);
    }
    const u32 orphan_keys = RemoveOrphanKeys(dir);
    if (verbose) {
        for (const auto& [why, n] : reasons) {
            std::printf("  %u: %s\n", n, why.c_str());
        }
    }
    // Keys of a stage compiled beside another fragment shader, made buildable (FixKeys), in a child.
    unsigned fixed = 0, fix_compiled = 0, fix_failed = 0;
    {
#ifdef _WIN32
        DWORD fix_code = 0;
        const std::string fix_out = RunSelf(self, L"--fix-keys \"" + dir.wstring() + L"\"", fix_code);
#else
        int fix_code = 0;
        const std::string fix_out = RunSelf(self, "--fix-keys " + ShellQuote(dir.string()), fix_code);
#endif
        if (const auto at = fix_out.find("@FIXED "); at != std::string::npos) {
            std::sscanf(fix_out.c_str() + at, "@FIXED %u %u %u", &fixed, &fix_compiled, &fix_failed);
        }
        (void)fix_code;
    }
    std::printf("Shader cache: %zu shaders recompiled for this engine, %zu left out (changed or "
                "without a state), %zu the compiler refused, %u pipeline keys without their shaders "
                "removed, %u keys of an older engine made buildable at the start (%u shaders compiled "
                "for them, %u not)\n",
                kept, left, crashed, orphan_keys, fixed, fix_compiled, fix_failed);
    return 0;
}

// --- What the engine's warm-up would build (bb-shader-lab --check-cache <cache folder>) ----------
// The records of each pipeline key read as PipelineCache::WarmUp reads them, in its order, without
// a GPU: the reason each key would be left out is counted, so a cache from a player's PC shows why
// its pipelines compiled in play instead of at the start.

// The descriptors a SPIR-V module declares in set 0: binding -> kind ('U' uniform buffer, 'S'
// storage buffer, 'I' sampled image, 'W' storage image, 'P' sampler), arrays by their element.
std::map<u32, char> SpirvBindings(const std::vector<char>& bytes) {
    std::map<u32, char> out;
    if (bytes.size() < 20 || bytes.size() % 4) return out;
    std::vector<u32> w(bytes.size() / 4);
    std::memcpy(w.data(), bytes.data(), bytes.size());
    std::map<u32, u32> binding, set, var_type, ptr_class, ptr_type, elem;
    std::map<u32, char> type_kind;
    for (size_t at = 5; at < w.size();) {
        const u32 count = w[at] >> 16, op = w[at] & 0xffff;
        if (!count || at + count > w.size()) break;
        const u32* a = &w[at + 1];
        if (op == 71 && count >= 4 && a[1] == 33) binding[a[0]] = a[2];       // OpDecorate Binding
        else if (op == 71 && count >= 4 && a[1] == 34) set[a[0]] = a[2];      // DescriptorSet
        else if (op == 59 && count >= 4) var_type[a[1]] = a[0];               // OpVariable
        else if (op == 32 && count >= 4) { ptr_class[a[0]] = a[1]; ptr_type[a[0]] = a[2]; } // OpTypePointer
        else if (op == 25 && count >= 9) type_kind[a[0]] = a[6] == 2 ? 'W' : 'I'; // OpTypeImage Sampled
        else if (op == 26 && count >= 2) type_kind[a[0]] = 'P';                // OpTypeSampler
        else if (op == 27 && count >= 3) type_kind[a[0]] = 'I';                // OpTypeSampledImage
        else if ((op == 28 || op == 29) && count >= 3) elem[a[0]] = a[1];      // arrays
        at += count;
    }
    for (const auto& [var, b] : binding) {
        if (set.count(var) && set[var] != 0) continue;
        const auto vt = var_type.find(var);
        if (vt == var_type.end()) continue;
        const u32 cls = ptr_class[vt->second];
        u32 pointee = ptr_type[vt->second];
        while (elem.count(pointee)) pointee = elem[pointee];
        char kind = '?';
        if (cls == 2) kind = 'U';            // Uniform
        else if (cls == 12) kind = 'S';      // StorageBuffer
        else if (cls == 0 && type_kind.count(pointee)) kind = type_kind[pointee]; // UniformConstant
        out[b] = kind;
    }
    return out;
}

// The pipeline layout the engine builds from these stage infos (GraphicsPipeline::
// BuildDescSetLayout): binding -> (kind, stage).
std::map<u32, std::pair<char, u32>> LayoutBindings(const std::array<const Info*, 5>& stages) {
    std::map<u32, std::pair<char, u32>> out;
    u32 binding = 0;
    for (u32 s = 0; s < stages.size(); ++s) {
        const Info* info = stages[s];
        if (!info) continue;
        for (const auto& buffer : info->buffers) out[binding++] = {buffer.is_uniform ? 'U' : 'S', s};
        for (const auto& image : info->images) {
            const u32 n = image.NumBindings(*info);
            for (u32 i = 0; i < n; ++i) out[binding + i] = {image.is_written ? 'W' : 'I', s};
            binding += n;
        }
        for (size_t i = 0; i < info->samplers.size(); ++i) out[binding++] = {'P', s};
    }
    return out;
}

// What a stage's module uses that the layout lacks, or has as another kind or for another stage.
u32 LayoutMismatches(const std::map<u32, char>& used, const std::map<u32, std::pair<char, u32>>& layout,
                     u32 stage, std::string* first) {
    u32 bad = 0;
    for (const auto& [b, kind] : used) {
        const auto it = layout.find(b);
        const bool ok = it != layout.end() && it->second.first == kind && it->second.second == stage;
        if (!ok) {
            if (first && first->empty()) {
                *first = "stage " + std::to_string(stage) + " binding " + std::to_string(b) + " " + kind +
                         (it == layout.end() ? " not in the layout"
                                             : std::string(" is ") + it->second.first + " of stage " +
                                                   std::to_string(it->second.second));
            }
            ++bad;
        }
    }
    return bad;
}

int CheckCache(const fs::path& dir) {
    std::vector<fs::path> keys;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(dir, error)) {
        if (entry.path().extension() == ".key") keys.push_back(entry.path());
    }
    std::sort(keys.begin(), keys.end());
    // What the warm-up's program cache holds: per program, its loaded permutations' specializations.
    std::map<u64, std::unique_ptr<Info>> infos;
    std::map<std::pair<u64, size_t>, StageSpecialization> loaded;
    std::map<std::string, u32> reasons;
    u32 good = 0;
    // Layout check: each pipeline's layout as the warm-up builds it (the program's first loaded
    // Info for every permutation) and with each permutation's own Info, against its SPIR-V.
    std::map<std::pair<u64, size_t>, std::unique_ptr<Info>> own_infos;
    u32 warmup_bad = 0, own_bad = 0, checked = 0;
    std::map<std::string, u32> warmup_examples;
    for (const auto& path : keys) {
        const auto key = ReadAll(path);
        std::array<const Info*, 5> warmup_stages{}, own_stages{};
        std::array<std::map<u32, char>, 5> used{};
        Backend::Bindings expected{};
        bool fs_motion = false, vs_motion = false;
        u32 version = 0, is_compute = 0;
        if (key.size() >= 8) {
            std::memcpy(&version, key.data(), 4);
            std::memcpy(&is_compute, key.data() + 4, 4);
        }
        if (!(version == 5 || version == 6) || is_compute > 1) { // PipelineKeyVersion 6, legacy 5
            ++reasons["key version " + std::to_string(version)];
            continue;
        }
        std::string why;
        const auto hashes = KeyStageHashes(key);
        for (size_t stage = 0; stage < hashes.size() && why.empty(); ++stage) {
            const u64 hash = hashes[stage];
            if (!hash) continue;
            char name[40];
            std::snprintf(name, sizeof(name), "0x%016llx.meta", (unsigned long long)hash);
            const auto meta = ReadAll(dir / name);
            if (meta.empty()) {
                why = "no shader record (.meta)";
                break;
            }
            Serialization::Archive ar{std::vector<u8>(meta.begin(), meta.end())};
            auto info = std::make_unique<Info>();
            StageSpecialization spec{};
            spec.info = info.get();
            std::optional<Gcn::FetchShaderData> fetch;
            size_t perm = 0;
            try {
                if (!Vulkan::LoadShaderMeta(ar, *info, fetch, spec, perm, hash)) {
                    // Which of LoadShaderMeta's checks: read again step by step.
                    Serialization::Archive again{std::vector<u8>(meta.begin(), meta.end())};
                    Serialization::Reader reader{again};
                    u32 meta_version = 0, binary_version = 0;
                    u64 perm_hash = 0;
                    size_t perm_idx = 0;
                    reader.Read(meta_version);
                    reader.Read(binary_version);
                    reader.Read(perm_hash);
                    reader.Read(perm_idx);
                    Info info2;
                    StageSpecialization spec2{};
                    spec2.info = &info2;
                    if (meta_version != 8) { // ShaderMetaVersion
                        why = "shader record version " + std::to_string(meta_version);
                    } else if (binary_version != 7 && // ShaderBinaryVersion, MotionShaderBinaryVersion
                               binary_version != 8) {
                        why = "shader binary version " + std::to_string(binary_version);
                    } else if (!spec2.Deserialize(again)) {
                        why = "specialization unreadable";
                    } else if (!info2.Deserialize(again)) {
                        bool zeros = true;
                        for (size_t i = 0; i < again.Remaining(); ++i) zeros = zeros && again.CurrPtr()[i] == 0;
                        why = std::string("shader info unreadable (srt/table plan), ") +
                              (zeros ? "zero bytes after it" : "other bytes after it");
                    } else if (perm_idx >= 4096 || perm_hash != hash ||
                               HashCombine(info2.pgm_hash, perm_idx) != hash) {
                        why = "hash mismatch";
                    } else {
                        why = "binary version " + std::to_string(binary_version) + " for motion " +
                              std::to_string(info2.hw_stage == HwStage::Vertex &&
                                             spec2.runtime_info.hw.vs.motion_vectors);
                    }
                    break;
                }
            } catch (const std::exception& e) {
                why = std::string("shader record unreadable: ") + e.what();
                break;
            }
            char spv_name[48];
            std::snprintf(spv_name, sizeof(spv_name), "0x%016llx_%zu.spv", (unsigned long long)info->pgm_hash, perm);
            const auto spv = ReadAll(dir / spv_name);
            u32 magic = 0;
            if (spv.size() >= 20) std::memcpy(&magic, spv.data(), 4);
            if (spv.size() < 20 || magic != 0x07230203) {
                why = "no SPIR-V (.spv)";
                break;
            }
            if (u32(info->sw_stage) != (is_compute ? u32(SwStage::Compute) : u32(stage))) {
                why = "shader of another stage";
                break;
            }
            const auto at = std::make_pair(info->pgm_hash, perm);
            if (!is_compute && stage < 5) {
                used[stage] = SpirvBindings(spv);
                auto& own = own_infos[at];
                if (!own) own = std::make_unique<Info>(*info);
                own_stages[stage] = own.get();
                // As the warm-up checks (LoadGraphicsPipeline): the stage's bindings start where
                // the stages before it end, and none is bound per mip level.
                bool mips = false;
                for (const auto& image : info->images)
                    mips |= image.mip_fallback_mode == MipStorageFallbackMode::DynamicIndex;
                if (mips) {
                    why = "an image bound per mip level (built in play)";
                    break;
                }
                if (spec.start.unified != expected.unified || spec.start.buffer != expected.buffer) {
                    why = "a stage compiled after another fragment shader (binding start)";
                    break;
                }
                expected.buffer += u32(info->buffers.size());
                expected.unified += u32(info->buffers.size() + info->images.size() + info->samplers.size());
                if (stage == u32(SwStage::Fragment)) fs_motion = spec.runtime_info.hw.fs.motion_vectors;
                if (stage == u32(SwStage::Vertex))
                    vs_motion = spec.runtime_info.hw_stage == HwStage::Vertex && spec.runtime_info.hw.vs.motion_vectors;
            }
            if (const auto it = loaded.find(at); it != loaded.end()) {
                spec.info = it->second.info;
                if (!(it->second == spec)) {
                    std::string what = "permutation loaded before with another specialization:";
                    if (!(spec.runtime_info == it->second.runtime_info)) what += " runtime";
                    if (spec.vs_attribs != it->second.vs_attribs) what += " vs_attribs";
                    if (spec.fetch_shader_data != it->second.fetch_shader_data) what += " fetch";
                    if (spec.start != it->second.start) what += " start";
                    if (spec.bitset != it->second.bitset) what += " bitset";
                    why = what;
                    break;
                }
            } else {
                auto& owner = infos[info->pgm_hash];
                if (!owner) owner = std::move(info);
                spec.info = owner.get();
                loaded.emplace(at, std::move(spec));
            }
            if (!is_compute && stage < 5) warmup_stages[stage] = infos[at.first].get();
        }
        if (why.empty() && fs_motion && !vs_motion) {
            why = "motion vectors read by the fragment stage, not written by the vertex stage";
        }
        if (why.empty()) {
            ++good;
            if (!is_compute) {
                ++checked;
                const auto warmup_layout = LayoutBindings(warmup_stages);
                const auto own_layout = LayoutBindings(own_stages);
                std::string first;
                u32 w = 0, o = 0;
                for (u32 s = 0; s < 5; ++s) {
                    w += LayoutMismatches(used[s], warmup_layout, s, &first);
                    o += LayoutMismatches(used[s], own_layout, s, nullptr);
                }
                if (w) {
                    ++warmup_bad;
                    if (warmup_examples.size() < 6) ++warmup_examples[path.filename().string() + ": " + first];
                    // BB_LAB_VERBOSE=1: the layout and what each stage's module uses.
                    if (const char* v = std::getenv("BB_LAB_VERBOSE"); v && v[0] == '1' && warmup_bad <= 4) {
                        std::string text = path.filename().string() + "\n    layout:";
                        for (const auto& [b, ks] : warmup_layout)
                            text += " " + std::to_string(b) + ks.first + std::to_string(ks.second);
                        for (u32 s = 0; s < 5; ++s) {
                            if (!warmup_stages[s]) continue;
                            const Info& i = *warmup_stages[s];
                            char head[160];
                            std::snprintf(head, sizeof(head), "\n    stage %u pgm %#llx: %zu buffers, %zu images, %zu samplers; uses:",
                                          s, (unsigned long long)i.pgm_hash, i.buffers.size(), i.images.size(), i.samplers.size());
                            text += head;
                            for (const auto& [b, k] : used[s]) text += " " + std::to_string(b) + k;
                        }
                        std::printf("%s\n", text.c_str());
                    }
                }
                if (o) ++own_bad;
            }
        } else {
            ++reasons[why];
        }
    }
    std::printf("Cache check: %u of %zu pipeline keys would be built at the start\n", good, keys.size());
    for (const auto& [why, n] : reasons) {
        std::printf("  %u: %s\n", n, why.c_str());
    }
    // A pipeline whose layout lacks what its SPIR-V uses is invalid Vulkan: NVIDIA's driver builds
    // it, AMD's ends the process (0.9.37 beta 6), and its draws read the wrong descriptors.
    std::printf("Layout check: %u of %u graphics pipelines have a layout that does not match their "
                "shaders as the warm-up builds them; %u with each permutation's own shader info\n",
                warmup_bad, checked, own_bad);
    for (const auto& [example, n] : warmup_examples) {
        std::printf("  %s\n", example.c_str());
    }
    return warmup_bad ? 4 : 0;
}

// --- The compiler's speed and output (docs/native_shader_compiler.md, "Compile speed") -----------
//   bb-shader-lab --bench <cache folder> [rounds] [report.csv] [--spv-dir DIR]
// Every shader state of a cache compiled in this process `rounds` times (3 by default) with the
// cache's profile, as the engine's CompileModule does: each shader's fastest round per phase
// (CompileStats), its IR and SPIR-V size and a hash of its SPIR-V, so a compiler change shows its
// time and which modules it changed. Nothing in the cache is written.

int Bench(const fs::path& dir, int rounds, const fs::path& report) {
    const auto profile_bytes = ReadAll(dir / "profile.bin");
    Profile profile{};
    if (profile_bytes.size() != sizeof(Profile)) {
        std::fprintf(stderr, "%s: profile of another engine\n", (dir / "profile.bin").string().c_str());
        return 3;
    }
    std::memcpy(&profile, profile_bytes.data(), sizeof(Profile));
    if (!ApplyLabProfile(profile)) {
        return 2;
    }
    struct Entry {
        std::string name;
        LoadedCapture cap;
        StageSpecialization spec;
        u32 stage = 0;
        size_t gcn_dwords = 0;
        std::array<unsigned long long, CompileStats::Count> best{};
        unsigned long long best_total = ~0ull;
        size_t ir_insts = 0, spv_words = 0;
        u64 spv_hash = 0;
        bool spec_same = false;
    };
    std::vector<std::unique_ptr<Entry>> entries;
    for (const auto& name : SpvNames(dir)) {
        unsigned long long pgm = 0;
        size_t perm = 0;
        if (std::sscanf(name.c_str(), "0x%llx_%zu", &pgm, &perm) != 2) {
            continue;
        }
        auto e = std::make_unique<Entry>();
        size_t meta_perm = ~size_t{};
        const auto meta = ReadAll(MetaPath(dir, pgm, perm));
        if (meta.empty() ||
            !Vulkan::ReadShaderMetaSpec(std::vector<u8>(meta.begin(), meta.end()), e->spec, meta_perm) ||
            meta_perm != perm || e->cap.Load(dir / (name + Native::StateExtension)) ||
            e->cap.code.empty()) {
            continue;
        }
        e->name = name;
        e->stage = e->cap.header.hw_stage;
        e->gcn_dwords = e->cap.code.size();
        e->cap = {};
        entries.push_back(std::move(e));
    }
    // Phase times are booked by the compiler itself; its 5 s report line stays off.
    CompileStats::last_report_ns = 1ll << 62;
    Pools pools;
    for (int round = 0; round < rounds; ++round) {
        for (auto& e : entries) {
            // Loaded again for each compile: a capture's tables sit at fixed low addresses.
            auto& cap = e->cap;
            cap = {};
            cap.Load(dir / (e->name + Native::StateExtension));
            Gcn::fetch_override = cap.fetch.empty() && cap.header.fetch_sgpr != ~0u && e->spec.fetch_shader_data
                                      ? &*e->spec.fetch_shader_data
                                      : nullptr;
            std::array<unsigned long long, CompileStats::Count> before{};
            for (int p = 0; p < CompileStats::Count; ++p) {
                before[p] = CompileStats::phase_ns[p].load();
            }
            ShaderParams params = cap.Params();
            Info info(HwStage(cap.header.hw_stage), SwStage(cap.header.sw_stage), params);
            RuntimeInfo runtime_info = cap.Runtime();
            const auto program = TranslateProgram(cap.code, pools, info, runtime_info, profile);
            Backend::Bindings bindings = e->spec.start;
            const auto spv = Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, bindings);
            std::array<unsigned long long, CompileStats::Count> took{};
            unsigned long long total = 0;
            for (int p = 0; p < CompileStats::Count; ++p) {
                took[p] = CompileStats::phase_ns[p].load() - before[p];
                total += took[p];
            }
            if (total < e->best_total) {
                e->best_total = total;
                e->best = took;
            }
            if (round == 0) {
                for (const IR::Block* block : program.blocks) {
                    e->ir_insts += block->Instructions().size();
                }
                e->spv_words = spv.size();
                e->spv_hash = 0xcbf29ce484222325ull; // FNV-1a over the words
                for (const u32 w : spv) {
                    e->spv_hash = (e->spv_hash ^ w) * 0x100000001b3ull;
                }
                const StageSpecialization new_spec(info, runtime_info, profile, e->spec.start);
                e->spec.info = &info;
                e->spec_same = new_spec == e->spec;
                if (!g_spv_dir.empty()) {
                    std::ofstream out(g_spv_dir / (e->name + ".spv"), std::ios::binary);
                    out.write(reinterpret_cast<const char*>(spv.data()), std::streamsize(spv.size() * 4));
                }
            }
        }
    }
    Gcn::fetch_override = nullptr;
    static constexpr const char* kPhase[] = {"decode", "translate", "ssa", "resources", "passes",
                                             "structurize", "late", "spirv"};
    std::ofstream csv(report);
    csv << "name,stage,gcn_dwords,ir_insts,spv_words,spv_hash,spec_same,total_us";
    for (const char* p : kPhase) csv << ',' << p << "_us";
    csv << '\n';
    std::array<double, CompileStats::Count> sum{};
    double all = 0;
    size_t words = 0, insts = 0, changed_spec = 0;
    std::vector<std::pair<double, std::string>> slowest;
    for (const auto& e : entries) {
        char line[256];
        std::snprintf(line, sizeof(line), "%s,%u,%zu,%zu,%zu,%016llx,%d,%.1f", e->name.c_str(),
                      e->stage, e->gcn_dwords, e->ir_insts, e->spv_words,
                      (unsigned long long)e->spv_hash, e->spec_same ? 1 : 0, e->best_total / 1e3);
        csv << line;
        for (int p = 0; p < CompileStats::Count; ++p) {
            csv << ',' << e->best[p] / 1e3;
            sum[p] += e->best[p] / 1e6;
        }
        csv << '\n';
        all += e->best_total / 1e6;
        words += e->spv_words;
        insts += e->ir_insts;
        changed_spec += e->spec_same ? 0 : 1;
        slowest.push_back({e->best_total / 1e6, e->name});
    }
    std::sort(slowest.rbegin(), slowest.rend());
    std::printf("Bench: %zu shaders, best of %d rounds: %.1f ms (%.2f avg)\n", entries.size(), rounds,
                all, entries.empty() ? 0.0 : all / entries.size());
    std::printf(" ");
    for (int p = 0; p < CompileStats::Count; ++p) {
        std::printf(" %s %.1f", kPhase[p], sum[p]);
    }
    std::printf(" ms\n  IR instructions %zu, SPIR-V words %zu, specializations changed %zu\n", insts,
                words, changed_spec);
    std::printf("  slowest:");
    for (size_t i = 0; i < std::min<size_t>(5, slowest.size()); ++i) {
        std::printf(" %s %.1f ms;", slowest[i].second.c_str(), slowest[i].first);
    }
    std::printf("\nPer shader: %s\n", report.string().c_str());
    return 0;
}

// --- The shader catalog ----------------------------------------------------------------------------
// A cache folder without anything taken from the game: each permutation's specialization (.meta),
// the pipeline keys (.key) and the draw states (.bbs) with no shader or fetch shader code. A
// player's PC puts the code back from its own game files (the shader bundles in
// dvdroot_ps4/shader and the executable), by the hash in each program's OrbShdr footer.

std::vector<u8> ReadBytes(const fs::path& path) {
    const auto v = ReadAll(path);
    return {v.begin(), v.end()};
}

bool WriteBytes(const fs::path& path, const std::vector<u8>& bytes) {
    return WriteAtomically(path, bytes.data(), bytes.size());
}

/// The layout offsets of a state file's parts (header, RuntimeInfo, user data, code, fetch, rest).
struct StateLayout {
    Native::CaptureHeader header{};
    size_t code_at = 0, fetch_at = 0, rest_at = 0;
    bool Parse(const std::vector<u8>& b) {
        if (b.size() < sizeof(header)) return false;
        std::memcpy(&header, b.data(), sizeof(header));
        if (std::memcmp(header.magic, Native::CaptureMagic, 4) != 0) return false;
        code_at = sizeof(header) + header.runtime_info_size + size_t(header.user_data_count) * 4;
        fetch_at = code_at + size_t(header.code_dwords) * 4;
        rest_at = fetch_at + size_t(header.fetch_dwords) * 4;
        return rest_at <= b.size();
    }
};

int ExportCatalog(const fs::path& cache, const fs::path& out) {
    std::error_code error;
    fs::create_directories(out, error);
    u32 states = 0, metas = 0, keys = 0, dead_keys = 0;
    std::string listed; // files.txt: the files of this catalog, one a line
    const auto has_meta = [&](u64 hash) {
        char name[40];
        std::snprintf(name, sizeof(name), "0x%016llx.meta", (unsigned long long)hash);
        return fs::exists(cache / name, error);
    };
    for (const auto& entry : fs::directory_iterator(cache, error)) {
        const auto& path = entry.path();
        const auto ext = path.extension();
        // Only keys a player's start can build: every stage's specialization is in the catalog.
        if (ext == ".key") {
            const auto hashes = KeyStageHashes(ReadAll(path));
            if (hashes.empty() || std::ranges::any_of(hashes, [&](u64 h) { return h && !has_meta(h); })) {
                ++dead_keys;
                continue;
            }
        }
        // A state without its specialization is never seeded.
        if (ext == Native::StateExtension) {
            unsigned long long pgm = 0;
            size_t perm = 0;
            if (std::sscanf(path.stem().string().c_str(), "0x%llx_%zu", &pgm, &perm) != 2 ||
                !fs::exists(MetaPath(cache, pgm, perm), error)) {
                continue;
            }
        }
        if (ext == ".key" || ext == ".meta" || path.filename() == "profile.bin") {
            fs::copy_file(path, out / path.filename(), fs::copy_options::overwrite_existing, error);
            listed += path.filename().string() + "\n";
            ext == ".key" ? ++keys : ext == ".meta" ? ++metas : 0;
        } else if (ext == Native::StateExtension) {
            auto bytes = ReadBytes(path);
            StateLayout layout;
            if (!layout.Parse(bytes)) continue;
            // Without the code and the fetch shader's code: what is left is the draw's state.
            std::vector<u8> stripped(bytes.begin(), bytes.begin() + layout.code_at);
            stripped.insert(stripped.end(), bytes.begin() + layout.rest_at, bytes.begin() + CopyTrailerAt(bytes));
            auto header = layout.header;
            header.code_dwords = 0;
            header.fetch_dwords = 0;
            std::memcpy(stripped.data(), &header, sizeof(header));
            if (WriteBytes(out / path.filename(), stripped)) {
                ++states;
                listed += path.filename().string() + "\n";
            }
        }
    }
    // bbport: the GPU vendor the catalog was made on, from the driver cache beside the folder
    // (pipeline_cache_blob.h: "BPBC", version, vendor, device): bb-start puts the catalog's profile
    // into a first start's empty cache only on a PC with a GPU of that vendor.
    auto driver_cache = cache;
    driver_cache += ".vkcache";
    if (const auto blob = ReadBytes(driver_cache); blob.size() >= 16 && std::memcmp(blob.data(), "BPBC", 4) == 0) {
        u32 vendor = 0, device = 0;
        std::memcpy(&vendor, blob.data() + 8, 4);
        std::memcpy(&device, blob.data() + 12, 4);
        char text[64];
        const int size = std::snprintf(text, sizeof(text), "vendor %04X device %04X\n", vendor, device);
        WriteAtomically(out / "gpu.txt", text, size_t(size));
    }
    // bbport: an update copies a package over the old one, so an older catalog's files can stay
    // beside this one's: --seed-cache reads only the files listed here.
    WriteAtomically(out / "files.txt", listed.data(), listed.size());
    std::printf("Shader catalog: %u states, %u specializations, %u pipeline keys in %s (%u keys without "
                "their shaders left out)\n",
                states, metas, keys, out.string().c_str(), dead_keys);
    return 0;
}

/// Every Gnm program in the game's files by its hash: the shader bundles and the executable.
struct CodeIndex {
    std::vector<std::vector<u8>> blobs;
    std::map<u64, std::pair<size_t, size_t>> programs; ///< hash -> blob, byte offset of the code
    /// bbport: geometry shaders' copy shaders (BinaryInfo::type 6) by hash: a geometry shader
    /// (type 5) and its copy shader carry the same hash, and the first one found was taken for
    /// both (a geometry shader seeded with its copy shader's code crashed the compiler).
    std::map<u64, std::pair<size_t, size_t>> copies;

    static std::vector<u8> Undcx(std::vector<u8>&& data) {
        if (data.size() < 0x4c || std::memcmp(data.data(), "DCX", 4) != 0) return std::move(data);
        if (std::memcmp(data.data() + 0x28, "DFLT", 4) != 0) return {};
        const auto be32 = [&](size_t at) {
            return u32(data[at]) << 24 | u32(data[at + 1]) << 16 | u32(data[at + 2]) << 8 | data[at + 3];
        };
        const auto dca = std::search(data.begin(), data.end(), "DCA", "DCA" + 4) - data.begin();
        if (size_t(dca) + 8 > data.size()) return {};
        const size_t skip = be32(size_t(dca) + 4);
        mz_ulong size = be32(0x1c);
        std::vector<u8> out(size);
        if (mz_uncompress(out.data(), &size, data.data() + dca + skip, mz_ulong(data.size() - dca - skip)) != MZ_OK) {
            return {};
        }
        out.resize(size);
        return out;
    }

    void Add(std::vector<u8>&& blob) {
        const size_t index = blobs.size();
        static constexpr char kSig[] = "OrbShdr";
        for (size_t at = 0; (at = std::string_view(reinterpret_cast<const char*>(blob.data()), blob.size()).find(kSig, at)) != std::string_view::npos; ++at) {
            if (at + 28 > blob.size()) break;
            u64 hash;
            std::memcpy(&hash, blob.data() + at + 16, 8);
            // The program starts with s_mov_b32 vcc_hi, #imm whose immediate puts the footer at
            // code + (imm + 1) * 8 bytes (AmdGpu::SearchBinaryInfo).
            for (size_t p = at & ~size_t(3); p >= 8 && at - p < 0x40000; p -= 4) {
                u32 token, imm;
                std::memcpy(&token, blob.data() + p, 4);
                std::memcpy(&imm, blob.data() + p + 4, 4);
                if (token == 0xBEEB03FFu && p + (size_t(imm) + 1) * 8 == at) {
                    u32 bits;
                    std::memcpy(&bits, blob.data() + at + 8, 4);
                    ((bits >> 2 & 15) == 6 ? copies : programs).try_emplace(hash, index, p);
                    break;
                }
            }
        }
        blobs.push_back(std::move(blob));
    }

    /// The program's code as the engine reads it (BinaryInfo::length bytes from its start).
    std::vector<u32> Code(u64 hash, bool copy_shader = false) const {
        auto it = programs.find(hash);
        if (copy_shader) {
            if (const auto copy = copies.find(hash); copy != copies.end()) it = copy;
        }
        if (it == programs.end()) return {};
        const auto& blob = blobs[it->second.first];
        const size_t start = it->second.second;
        u32 imm;
        std::memcpy(&imm, blob.data() + start + 4, 4);
        const size_t footer = start + (size_t(imm) + 1) * 8;
        u32 bits;
        std::memcpy(&bits, blob.data() + footer + 8, 4);
        const size_t length = bits >> 8;
        if (start + length > blob.size()) return {};
        std::vector<u32> code(length / 4);
        std::memcpy(code.data(), blob.data() + start, code.size() * 4);
        return code;
    }
};

/// The permutation numbers the cache uses for a program.
std::set<size_t> UsedPerms(const fs::path& cache, u64 pgm) {
    std::set<size_t> used;
    char prefix[32];
    std::snprintf(prefix, sizeof(prefix), "0x%016llx_", (unsigned long long)pgm);
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(cache, error)) {
        const auto name = entry.path().stem().string();
        if (name.rfind(prefix, 0) == 0) used.insert(std::strtoull(name.c_str() + std::strlen(prefix), nullptr, 10));
    }
    return used;
}

int SeedCache(const fs::path& self, const fs::path& catalog, const fs::path& cache, const fs::path& game,
              const fs::path& eboot) {
    std::error_code error;
    // Only into a cache made on a GPU with the catalog's profile: another profile gives other
    // specializations, and the engine would start a cache of another profile again anyway.
    // bbport: only the GPU's part of the profile, the fields before the engine's own compile
    // switches (uniform buffers on, their layout...). Those switches grew the profile in 0.9.34
    // beta 9.9 and every catalog shipped since was refused on every PC (docs/pipeline_compiles.md).
    // A catalog permutation compiled with other switches is kept only if this compiler, with this
    // cache's switches, specializes it the same (RecompileBatch); the others are left out.
    const auto catalog_profile = ReadBytes(catalog / "profile.bin");
    const auto cache_profile = ReadBytes(cache / "profile.bin");
    static constexpr size_t GpuBytes = offsetof(Profile, uniform_constant_buffers);
    if (catalog_profile.size() < GpuBytes || cache_profile.size() < GpuBytes) {
        std::printf("Shader catalog: made on another kind of GPU than this one; not used\n");
        return 5;
    }
    // bbport (0.9.37 beta 6): another GPU of the catalog's vendor (an RTX 3080's profile differs
    // from the RTX 5060 the catalog was made on by one extension) gets it too: every permutation is
    // compiled with this cache's profile and kept only if its specialization comes out the same,
    // as for other compile switches. The vendors come from the catalog's gpu.txt and the driver
    // cache beside this cache (its first start writes it); pipeline state of another vendor (formats
    // its driver lacks) is not risked.
    if (std::memcmp(catalog_profile.data(), cache_profile.data(), GpuBytes) != 0) {
        unsigned catalog_vendor = 0;
        if (const auto text = ReadBytes(catalog / "gpu.txt"); !text.empty()) {
            std::sscanf(std::string(text.begin(), text.end()).c_str(), "vendor %x", &catalog_vendor);
        }
        u32 cache_vendor = 0;
        auto driver_cache = cache;
        driver_cache += ".vkcache";
        if (const auto blob = ReadBytes(driver_cache); blob.size() >= 16 && std::memcmp(blob.data(), "BPBC", 4) == 0) {
            std::memcpy(&cache_vendor, blob.data() + 8, 4);
        }
        if (catalog_vendor == 0 || catalog_vendor != cache_vendor) {
            std::printf("Shader catalog: made on another kind of GPU than this one; not used\n");
            return 5;
        }
        std::printf("Shader catalog: made on another GPU of this vendor (%04X); each shader compiled for this one\n",
                    catalog_vendor);
    }
    const bool other_switches = catalog_profile != cache_profile;
    // The catalog's own files (files.txt, since 0.9.37 beta 3): an older catalog's left beside
    // them by an update are not read.
    std::set<std::string> own;
    if (std::ifstream list{catalog / "files.txt"}) {
        for (std::string line; std::getline(list, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) own.insert(line);
        }
    }
    const auto in_catalog = [&](const fs::path& path) { return own.empty() || own.count(path.filename().string()) != 0; };
    CodeIndex index;
    for (const auto& entry : fs::directory_iterator(game / "dvdroot_ps4" / "shader", error)) {
        if (entry.path().extension() == ".dcx") index.Add(CodeIndex::Undcx(ReadBytes(entry.path())));
    }
    index.Add(ReadBytes(eboot));
    // Catalog permutation -> the number it gets here (a program's own permutations keep theirs).
    std::map<u64, u64> renamed_hashes;
    std::vector<std::string> added_names;
    u32 added = 0, missing = 0, present = 0, renumbered = 0, unreadable = 0;
    for (const auto& entry : fs::directory_iterator(catalog, error)) {
        if (entry.path().extension() != Native::StateExtension || !in_catalog(entry.path())) continue;
        const auto name = entry.path().stem().string();
        unsigned long long pgm = 0;
        size_t perm = 0;
        if (std::sscanf(name.c_str(), "0x%llx_%zu", &pgm, &perm) != 2) continue;
        auto bytes = ReadBytes(entry.path());
        StateLayout layout;
        auto meta = ReadBytes(MetaPath(catalog, pgm, perm));
        if (!layout.Parse(bytes) || meta.size() < 24) {
            ++unreadable;
            continue;
        }
        // bbport: a version 7 record (before 0.9.34 beta 9.9) differs from version 8 only by
        // HwFragmentRuntimeInfo::depth_first, then padding, which the state's RuntimeInfo carries
        // alike: the recompile compares the two as they are.
        if (u32 version; std::memcpy(&version, meta.data(), 4), version == 7) {
            version = 8;
            std::memcpy(meta.data(), &version, 4);
        }
        const auto code = index.Code(pgm);
        if (code.empty()) {
            ++missing; // not in this game's files (another edition)
            continue;
        }
        // A permutation this cache has already (same program, same specialization) is not added
        // again; a clash of numbers must not mix two specializations: the catalog's gets a free
        // number and its keys follow.
        const auto used = UsedPerms(cache, pgm);
        StageSpecialization catalog_spec;
        size_t catalog_perm = 0;
        // A record of another engine (its specialization written otherwise): re-export the catalog.
        if (!Vulkan::ReadShaderMetaSpec(std::vector<u8>(meta), catalog_spec, catalog_perm)) {
            ++unreadable;
            continue;
        }
        const Info valid_marker{}; // StageSpecialization::Valid() only asks for an Info
        catalog_spec.info = &valid_marker;
        bool have = false;
        for (const size_t mine : used) {
            StageSpecialization spec;
            size_t mine_perm = 0;
            const auto mine_meta = ReadBytes(MetaPath(cache, pgm, mine));
            if (!mine_meta.empty() && Vulkan::ReadShaderMetaSpec(std::vector<u8>(mine_meta), spec, mine_perm)) {
                spec.info = &valid_marker;
                if (spec == catalog_spec) {
                    have = true;
                    // The catalog's keys name it by the catalog's number: they follow this one.
                    renamed_hashes[HashCombine(u64(pgm), u64(perm))] = HashCombine(u64(pgm), u64(mine));
                    break;
                }
            }
        }
        if (have) {
            ++present;
            continue;
        }
        size_t target = perm;
        if (used.count(perm)) {
            ++renumbered;
            target = *used.rbegin() + 1;
            while (used.count(target)) ++target;
        }
        if (target >= 4096) continue;
        const u64 old_hash = HashCombine(u64(pgm), u64(perm));
        const u64 new_hash = HashCombine(u64(pgm), u64(target));
        renamed_hashes[old_hash] = new_hash;
        // The state with the code from this PC's game files.
        auto header = layout.header;
        header.code_dwords = u32(code.size());
        std::vector<u8> full(bytes.begin(), bytes.begin() + layout.code_at);
        full.insert(full.end(), reinterpret_cast<const u8*>(code.data()),
                    reinterpret_cast<const u8*>(code.data() + code.size()));
        full.insert(full.end(), bytes.begin() + layout.code_at, bytes.end());
        std::memcpy(full.data(), &header, sizeof(header));
        // A geometry shader's copy shader, by the hash in its RuntimeInfo, from the game's files.
        if (HwStage(header.hw_stage) == HwStage::Geometry && CopyTrailerAt(bytes) == bytes.size()) {
            alignas(RuntimeInfo) unsigned char runtime_storage[sizeof(RuntimeInfo)];
            std::memcpy(runtime_storage, bytes.data() + sizeof(header), sizeof(RuntimeInfo));
            const auto& runtime = *std::launder(reinterpret_cast<const RuntimeInfo*>(runtime_storage));
            const auto copy = index.Code(runtime.hw.gs.vs_copy_hash, true);
            if (copy.empty()) {
                ++missing;
                continue;
            }
            const u32 trailer[2] = {u32(copy.size()), Native::CopyTrailerMagic};
            full.insert(full.end(), reinterpret_cast<const u8*>(copy.data()),
                        reinterpret_cast<const u8*>(copy.data() + copy.size()));
            full.insert(full.end(), reinterpret_cast<const u8*>(trailer), reinterpret_cast<const u8*>(trailer + 2));
        }
        char target_name[48];
        std::snprintf(target_name, sizeof(target_name), "0x%016llx_%zu", pgm, target);
        // The meta's permutation fields (hash at 8, number at 16) for the new number; the
        // recompile writes the rest of it again.
        std::memcpy(meta.data() + 8, &new_hash, 8);
        const u64 target64 = target;
        std::memcpy(meta.data() + 16, &target64, 8);
        if (WriteBytes(cache / (std::string(target_name) + Native::StateExtension), full) &&
            WriteBytes(MetaPath(cache, pgm, target), meta)) {
            ++added;
            added_names.push_back(target_name);
        }
    }
    // The pipeline keys, with the stage hashes of renumbered permutations replaced.
    u32 keys = 0;
    for (const auto& entry : fs::directory_iterator(catalog, error)) {
        if (entry.path().extension() != ".key" || !in_catalog(entry.path()) ||
            fs::exists(cache / entry.path().filename(), error)) {
            continue;
        }
        auto key = ReadBytes(entry.path());
        for (const auto& [from, to] : renamed_hashes) {
            if (from == to) continue;
            for (size_t at = 0; at + 8 <= key.size(); at += 1) {
                u64 v;
                std::memcpy(&v, key.data() + at, 8);
                if (v == from) std::memcpy(key.data() + at, &to, 8);
            }
        }
        if (WriteBytes(cache / entry.path().filename(), key)) ++keys;
    }
    std::printf("Shader catalog: %u permutations added (%u renumbered), %u here already, %u not in this "
                "game's files, %u records of another engine, %u pipeline keys; compiling them%s\n",
                added, renumbered, present, missing, unreadable, keys,
                other_switches ? " (made with other compile switches: each one checked)" : "");
    if (added_names.empty()) {
        return 0;
    }
    const fs::path list = cache / "catalog-seed.txt";
    {
        std::ofstream out(list, std::ios::trunc);
        for (const auto& name : added_names) out << name << '\n';
    }
    const int code = RecompileCache(self, cache, list);
    fs::remove(list, error);
    return code;
}

// --- The native compiler's checks (docs/native_shader_compiler.md) -------------------------------
//   bb-shader-lab --census <capture folder> [report.csv]
//       what the game's shaders use: GCN opcodes, control flow shape, resources, resource tables
//   bb-shader-lab --tables <capture folder> [report.csv]
//       each capture's resource table walker against its table plan (native/table_plan.h): over
//       null tables, synthetic tables (two seeds) and the captured tables rebuilt from the plan

/// The capture folder's profile, with the --native options as RunOne sets them.
Profile CaptureProfile(const fs::path& file) {
    const auto bytes = ReadAll(file.parent_path() / "profile.bin");
    Profile profile{};
    if (bytes.size() >= 4) {
        std::memcpy(&profile, bytes.data() + 4, std::min<size_t>(bytes.size() - 4, sizeof(Profile)));
    }
    profile.uniform_constant_buffers = g_native;
    profile.uniform_dword_layout = g_native;
    profile.uniform_exact_base = false;
    return profile;
}

/// Immediate dominators (Cooper, Harvey, Kennedy) of the nodes reached from `entry` through
/// `out`; -1 for the others.
std::vector<int> Dominators(u32 count, u32 entry, const std::vector<std::vector<u32>>& out,
                            const std::vector<std::vector<u32>>& in) {
    std::vector<u32> post;
    std::vector<bool> seen(count);
    std::vector<std::pair<u32, size_t>> stack{{entry, 0}};
    seen[entry] = true;
    while (!stack.empty()) {
        const u32 v = stack.back().first;
        if (stack.back().second < out[v].size()) {
            const u32 w = out[v][stack.back().second++];
            if (!seen[w]) {
                seen[w] = true;
                stack.push_back({w, 0});
            }
        } else {
            post.push_back(v);
            stack.pop_back();
        }
    }
    std::vector<int> order(count, -1);
    for (size_t i = 0; i < post.size(); ++i) {
        order[post[i]] = int(post.size() - 1 - i);
    }
    std::vector<int> idom(count, -1);
    idom[entry] = int(entry);
    for (bool changed = true; changed;) {
        changed = false;
        for (auto it = post.rbegin(); it != post.rend(); ++it) {
            const u32 v = *it;
            if (v == entry) {
                continue;
            }
            int dom = -1;
            for (const u32 p : in[v]) {
                if (idom[p] < 0) {
                    continue;
                }
                if (dom < 0) {
                    dom = int(p);
                    continue;
                }
                int a = int(p), b = dom;
                while (a != b) {
                    while (order[a] > order[b]) a = idom[a];
                    while (order[b] > order[a]) b = idom[b];
                }
                dom = a;
            }
            if (dom >= 0 && idom[v] != dom) {
                idom[v] = dom;
                changed = true;
            }
        }
    }
    return idom;
}

/// The control flow shape of a shader's GCN blocks: what a structurizer has to rebuild. A shader
/// is structured (no goto variables needed) when its graph is reducible, each loop leaves to one
/// place, and each two-way branch opens a region nothing enters from the side.
struct FlowShape {
    u32 blocks = 0, branches = 0, exec_branches = 0, exits = 0, loops = 0, irreducible = 0,
        multi_exit_loops = 0, side_entries = 0, max_loop_depth = 0;
};

FlowShape AnalyzeFlow(std::span<const Gcn::GcnInst> insts) {
    FlowShape shape;
    Common::ObjectPool<Gcn::Block> pool{64};
    Gcn::CFG cfg{pool, insts};
    std::vector<const Gcn::Block*> blocks;
    std::map<const Gcn::Block*, u32> index;
    for (const auto& block : cfg) {
        index[&block] = u32(blocks.size());
        blocks.push_back(&block);
    }
    const u32 n = u32(blocks.size());
    shape.blocks = n;
    if (n == 0) {
        return shape;
    }
    // Node n: one exit after every s_endpgm, for the post-dominators.
    std::vector<std::vector<u32>> succ(n + 1), pred(n + 1);
    const auto link = [&](u32 a, u32 b) {
        succ[a].push_back(b);
        pred[b].push_back(a);
    };
    for (u32 i = 0; i < n; ++i) {
        const auto* block = blocks[i];
        if (block->end_class == Gcn::EndClass::Exit) {
            ++shape.exits;
            link(i, n);
            continue;
        }
        if (block->branch_true) link(i, index.at(block->branch_true));
        if (block->branch_false && block->branch_false != block->branch_true) {
            link(i, index.at(block->branch_false));
        }
        if (succ[i].size() == 2) {
            ++shape.branches;
            const auto op = block->end_inst.opcode;
            shape.exec_branches += op == Gcn::Opcode::S_CBRANCH_EXECZ || op == Gcn::Opcode::S_CBRANCH_EXECNZ;
        }
    }
    const auto idom = Dominators(n + 1, 0, succ, pred);
    const auto dominates = [&](u32 a, u32 b) {
        for (int v = int(b); v >= 0;) {
            if (u32(v) == a) return true;
            if (idom[v] == v) return false;
            v = idom[v];
        }
        return false;
    };
    // Retreating edges (to a block still on the depth-first path): loops when the target
    // dominates the source, else an irreducible graph.
    std::set<std::pair<u32, u32>> back;
    std::map<u32, std::vector<u32>> latches;
    {
        std::vector<u8> state(n + 1); // 0 new, 1 on the path, 2 done
        std::vector<std::pair<u32, size_t>> stack{{0, 0}};
        state[0] = 1;
        while (!stack.empty()) {
            const u32 v = stack.back().first;
            if (stack.back().second < succ[v].size()) {
                const u32 w = succ[v][stack.back().second++];
                if (state[w] == 0) {
                    state[w] = 1;
                    stack.push_back({w, 0});
                } else if (state[w] == 1) {
                    if (dominates(w, v)) {
                        back.insert({v, w});
                        latches[w].push_back(v);
                    } else {
                        ++shape.irreducible;
                    }
                }
            } else {
                state[v] = 2;
                stack.pop_back();
            }
        }
    }
    // Natural loops: each header's body, and where it leaves to.
    std::vector<std::vector<bool>> bodies;
    for (const auto& [header, from] : latches) {
        std::vector<bool> body(n + 1);
        body[header] = true;
        std::vector<u32> work(from.begin(), from.end());
        while (!work.empty()) {
            const u32 v = work.back();
            work.pop_back();
            if (body[v]) continue;
            body[v] = true;
            for (const u32 p : pred[v]) work.push_back(p);
        }
        std::set<u32> exits;
        for (u32 v = 0; v < n; ++v) {
            if (!body[v]) continue;
            for (const u32 w : succ[v]) {
                if (!body[w]) exits.insert(w);
            }
        }
        ++shape.loops;
        shape.multi_exit_loops += exits.size() > 1;
        bodies.push_back(std::move(body));
    }
    std::vector<int> innermost(n + 1, -1); // the smallest loop body holding each block
    std::vector<u32> loop_depth(n + 1, 0);
    for (size_t l = 0; l < bodies.size(); ++l) {
        const auto size = std::count(bodies[l].begin(), bodies[l].end(), true);
        for (u32 v = 0; v < n; ++v) {
            if (!bodies[l][v]) continue;
            shape.max_loop_depth = std::max(shape.max_loop_depth, ++loop_depth[v]);
            if (innermost[v] < 0 ||
                size < std::count(bodies[innermost[v]].begin(), bodies[innermost[v]].end(), true)) {
                innermost[v] = int(l);
            }
        }
    }
    // Post-dominators on the reversed graph, from the exit.
    const auto ipdom = Dominators(n + 1, n, pred, succ);
    for (u32 b = 0; b < n; ++b) {
        if (succ[b].size() != 2 || back.contains({b, succ[b][0]}) || back.contains({b, succ[b][1]})) {
            continue;
        }
        const int join = ipdom[b] < 0 ? int(n) : ipdom[b];
        const auto* loop = innermost[b] >= 0 ? &bodies[innermost[b]] : nullptr;
        std::vector<bool> region(n + 1);
        std::vector<u32> work(succ[b].begin(), succ[b].end());
        while (!work.empty()) {
            const u32 v = work.back();
            work.pop_back();
            if (v == n || int(v) == join || region[v] || v == b || (loop && !(*loop)[v])) continue;
            region[v] = true;
            for (const u32 w : succ[v]) {
                if (!back.contains({v, w})) work.push_back(w);
            }
        }
        bool side_entry = false;
        for (u32 v = 0; v < n && !side_entry; ++v) {
            if (!region[v]) continue;
            for (const u32 p : pred[v]) {
                if (p != b && !region[p] && !back.contains({p, v})) {
                    side_entry = true;
                    break;
                }
            }
        }
        shape.side_entries += side_entry;
    }
    return shape;
}

/// --ir: one capture's final IR, block by block (what the native passes match on).
int DumpIr(const fs::path& file) {
    Profile profile = CaptureProfile(file);
    LoadedCapture cap;
    if (const char* error = cap.Load(file)) {
        std::printf("%s: %s\n", file.filename().string().c_str(), error);
        return 2;
    }
    ShaderParams params = cap.Params();
    Info info(HwStage(cap.header.hw_stage), SwStage(cap.header.sw_stage), params);
    Pools pools;
    const auto program = TranslateProgram(cap.code, pools, info, cap.Runtime(), profile);
    for (const IR::Block* block : program.blocks) {
        std::printf("%s\n", IR::DumpBlock(*block).c_str());
    }
    return 0;
}


// --- The composite's tone curve (docs/hdr-scene-path.md, steps 1-2) ------------------------------
//   bb-shader-lab --curves <capture folder> [report.csv]
//       per capture: the tone curve channels found (native/tone_curve.h), whether they are a
//       composite, and that the extension changes the SPIR-V of the composites only
//   bb-shader-lab --ramp-modules <file.bbs> <out folder>
//       the composite compiled as the game's and with the extension, each with a ramp of scene
//       values (exp2 over the frame's width) into the curve and the curve's three channels out
//       (tests/test_tone_curve.cpp runs them on the GPU)

/// The scene ramp of --ramp-modules: x = 2^(RampLow + RampStops * (frag_x / RampWidth)).
constexpr float RampLow = -10.0f, RampStops = 24.0f;
constexpr u32 RampWidth = 4096;

/// One capture compiled, with the extension or without; `ramp`: the test rewrite above.
std::vector<u32> CompileComposite(const fs::path& file, bool extend, bool ramp,
                                  std::vector<Native::ToneCurveMatch>& matches, std::string& note) {
    Profile profile = CaptureProfile(file);
    LoadedCapture cap;
    if (const char* error = cap.Load(file)) {
        note = error;
        return {};
    }
    ShaderParams params = cap.Params();
    Info info(HwStage(cap.header.hw_stage), SwStage(cap.header.sw_stage), params);
    Pools pools;
    std::vector<IR::Inst*> channels;
    Native::tone_curve_lab_override = extend ? 1 : 0;
    Native::tone_curve_lab_channels = &channels;
    auto program = TranslateProgram(cap.code, pools, info, cap.Runtime(), profile);
    Native::tone_curve_lab_override = -1;
    Native::tone_curve_lab_channels = nullptr;
    matches = Native::FindToneCurves(program);
    if (ramp) {
        if (!Native::IsComposite(matches)) {
            note = "not a composite";
            return {};
        }
        IR::Block& block = *matches[0].result->GetParent();
        std::map<const IR::Inst*, size_t> order;
        size_t index = 0;
        for (IR::Inst& inst : block.Instructions()) {
            order[&inst] = index++;
        }
        std::ranges::sort(matches, [&](const auto& a, const auto& b) {
            return order.at(a.result) < order.at(b.result);
        });
        std::vector<IR::Inst*> outputs;
        if (extend) {
            if (channels.size() != 3) {
                note = "the extension did not apply";
                return {};
            }
            outputs = channels;
        } else {
            for (const auto& m : matches) {
                outputs.push_back(m.result);
            }
        }
        // The scene value of every channel: the ramp.
        for (const auto& m : matches) {
            IR::IREmitter ir{block, IR::Block::InstructionList::s_iterator_to(*m.scene)};
            const IR::F32 x = ir.GetAttribute(IR::Attribute::FragCoord, 0);
            const IR::F32 stops = IR::F32{ir.FPAdd(
                ir.Imm32(RampLow), IR::F32{ir.FPMul(x, ir.Imm32(RampStops / float(RampWidth)))})};
            const IR::Value scene = ir.FPExp2(stops);
            m.scene->SetArg(m.scene->Arg(0).IsImmediate() ? 1 : 0, scene);
        }
        info.loads.Set(IR::Attribute::FragCoord, 0);
        // The curve's channels out, alpha 1.
        for (IR::Inst& inst : block.Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::SetAttribute ||
                inst.Arg(0).Attribute() != IR::Attribute::RenderTarget0) {
                continue;
            }
            const u32 comp = inst.Arg(2).U32();
            inst.SetArg(1, comp < 3 ? IR::Value{outputs[comp]} : IR::Value{1.0f});
        }
        Optimization::DeadCodeEliminationPass(program);
    }
    Backend::Bindings bindings{};
    return Backend::SPIRV::EmitSPIRV(profile, cap.Runtime(), program, bindings);
}

/// --curves-one: "CURVES|channels=N;composite=0|1;family=..;words=a/b;changed=0|1".
int CurvesOne(const fs::path& file) {
    std::vector<Native::ToneCurveMatch> matches, unused;
    std::string note;
    const auto game = CompileComposite(file, false, false, matches, note);
    if (game.empty()) {
        std::printf("%s: %s\n", file.filename().string().c_str(), note.c_str());
        return 2;
    }
    const auto extended = CompileComposite(file, true, false, unused, note);
    const bool composite = Native::IsComposite(matches);
    // A shader that differs without being a composite: compiled again without the extension, to
    // tell the inherited compiler's run-to-run differences apart.
    bool varies = false;
    if (!composite && game != extended) {
        varies = CompileComposite(file, false, false, unused, note) != game ||
                 CompileComposite(file, false, false, unused, note) != extended;
    }
    const char* family = matches.empty() ? "-"
                         : matches[0].family == Native::ToneCurveMatch::Family::Exponential
                             ? "exponential"
                             : "reinhard";
    std::printf("CURVES|channels=%zu;composite=%u;family=%s;words=%u/%u;changed=%u;varies=%u\n",
                matches.size(), composite ? 1u : 0u, family,
                matches.empty() ? 0u : matches[0].k_word,
                matches.empty() ? 0u : matches[0].shoulder_word, game != extended ? 1u : 0u,
                varies ? 1u : 0u);
    return 0;
}

/// --ramp-modules: game.spv, hdr.spv and ramp.txt (family, constant words, the ramp, the constant
/// buffer's descriptor type) in `out`.
int RampModules(const fs::path& file, const fs::path& out) {
    std::vector<Native::ToneCurveMatch> matches, unused;
    std::string note;
    const auto game = CompileComposite(file, false, true, matches, note);
    const auto hdr = game.empty() ? game : CompileComposite(file, true, true, unused, note);
    if (game.empty() || hdr.empty()) {
        std::printf("%s: %s\n", file.filename().string().c_str(), note.c_str());
        return 1;
    }
    fs::create_directories(out);
    const auto write = [&](const char* name, const std::vector<u32>& words) {
        std::ofstream o(out / name, std::ios::binary);
        o.write(reinterpret_cast<const char*>(words.data()), std::streamsize(words.size() * 4));
    };
    write("game.spv", game);
    write("hdr.spv", hdr);
    std::ofstream meta(out / "ramp.txt");
    meta << (matches[0].family == Native::ToneCurveMatch::Family::Exponential ? "exponential"
                                                                             : "reinhard")
         << ' ' << matches[0].k_word << ' ' << matches[0].shoulder_word << ' ' << RampLow << ' '
         << RampStops << ' ' << RampWidth << ' ' << (g_native ? "uniform" : "storage") << '\n';
    std::printf("%s: ramp modules in %s\n", file.filename().string().c_str(), out.string().c_str());
    return 0;
}

/// --census-one: one capture's census, "CENSUS|key=value;..." and "GCN|opcode=count;...".
int CensusOne(const fs::path& file) {
    Profile profile = CaptureProfile(file);
    LoadedCapture cap;
    if (const char* error = cap.Load(file)) {
        std::printf("%s: %s\n", file.filename().string().c_str(), error);
        return 2;
    }
    ShaderParams params = cap.Params();
    Info info(HwStage(cap.header.hw_stage), SwStage(cap.header.sw_stage), params);
    Pools pools;
    Native::TablePlan plan;
    Native::TablePlan::lab_target = &plan;
    const auto program = TranslateProgram(cap.code, pools, info, cap.Runtime(), profile);
    Native::TablePlan::lab_target = nullptr;
    const FlowShape flow = AnalyzeFlow(program.ins_list);
    u32 ubo = 0, ssbo = 0, texel = 0, written = 0, special = 0;
    for (const auto& buffer : info.buffers) {
        if (buffer.IsSpecial()) {
            ++special;
        } else if (buffer.is_formatted) {
            ++texel;
        } else if (buffer.only_const_reads && !buffer.is_written) {
            ++ubo;
        } else {
            ++ssbo;
        }
        written += buffer.is_written;
    }
    // LDS: the shader's own DS reads and writes (ds_swizzle is a lane operation, ds_append and
    // ds_consume are GDS counters).
    u32 lds = 0;
    for (const auto& inst : program.ins_list) {
        const auto name = magic_enum::enum_name(inst.opcode);
        lds += name.starts_with("DS_READ") || name.starts_with("DS_WRITE");
    }
    u32 storage_images = 0, array_images = 0, depth_images = 0, native_images = 0;
    for (const auto& image : info.images) {
        storage_images += image.is_written || image.is_atomic;
        array_images += image.is_array;
        depth_images += image.is_depth;
        native_images += image.needs_native;
    }
    std::printf("CENSUS|stage=%u;gcn_insts=%zu;blocks=%u;branches=%u;exec_branches=%u;exits=%u;"
                "loops=%u;max_loop_depth=%u;irreducible=%u;multi_exit_loops=%u;side_entries=%u;"
                "const_buffers=%u;storage_buffers=%u;texel_buffers=%u;written_buffers=%u;"
                "special_buffers=%u;images=%zu;storage_images=%u;array_images=%u;depth_images=%u;"
                "native_images=%u;samplers=%zu;fmasks=%zu;flat_dwords=%u;table_roots=%u;tables=%u;"
                "table_depth=%u;dynamic_offsets=%u;plan_failed=%u;lds=%u;discard=%u;gather=%u;"
                "image_query=%u;fetch_shader=%u;lane_id=%u;quad=%u;shuffle=%u;ballot=%u;fp16=%u;"
                "fp64=%u;dma=%u\n",
                cap.header.hw_stage, program.ins_list.size(), flow.blocks, flow.branches,
                flow.exec_branches, flow.exits, flow.loops, flow.max_loop_depth, flow.irreducible,
                flow.multi_exit_loops, flow.side_entries, ubo, ssbo, texel, written, special,
                info.images.size(), storage_images, array_images, depth_images, native_images,
                info.samplers.size(), info.fmasks.size(), info.srt_info.flattened_bufsize_dw,
                plan.roots, plan.tables, plan.depth, plan.dynamic, plan.valid ? 0u : 1u,
                lds ? 1u : 0u, info.has_discard ? 1u : 0u,
                info.has_image_gather ? 1u : 0u, info.has_image_query ? 1u : 0u,
                info.has_fetch_shader ? 1u : 0u, info.uses_lane_id ? 1u : 0u,
                info.uses_group_quad ? 1u : 0u, info.uses_group_shuffle ? 1u : 0u,
                info.uses_group_ballot ? 1u : 0u, info.uses_fp16 ? 1u : 0u,
                info.uses_fp64 ? 1u : 0u, info.uses_dma ? 1u : 0u);
    if (!plan.valid) {
        std::printf("PLAN|%s\n", plan.error.c_str());
    }
    std::map<std::string_view, u32> ops;
    for (const auto& inst : program.ins_list) {
        ++ops[magic_enum::enum_name(inst.opcode)];
    }
    std::printf("GCN|");
    for (const auto& [name, count] : ops) {
        std::printf("%.*s=%u;", int(name.size()), name.data(), count);
    }
    std::printf("\n");
    return 0;
}

#ifdef _WIN32
/// Whether [at, at + bytes) is committed, readable memory of this process.
bool Readable(u64 at, u64 bytes) {
    const u64 end = at + bytes;
    if (end < at) {
        return false;
    }
    while (at < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi)) ||
            mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
            return false;
        }
        at = reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

/// The walker's faults handled as in the game (the read gives 0), through the handlers the
/// recompiler registers.
LONG CALLBACK WalkerFault(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        Core::Signals::Instance()->DispatchAccessViolation(
            ep, reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1]))) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/// --tables-one: one capture's walker against its plan. "TABLES|result;..." with result pass,
/// fail, "no tables" or "no plan".
int TablesOne(const fs::path& file) {
    Profile profile = CaptureProfile(file);
    LoadedCapture cap;
    if (const char* error = cap.Load(file)) {
        std::printf("%s: %s\n", file.filename().string().c_str(), error);
        return 2;
    }
    ShaderParams params = cap.Params();
    Info info(HwStage(cap.header.hw_stage), SwStage(cap.header.sw_stage), params);
    Pools pools;
    Native::TablePlan plan;
    Native::TablePlan::lab_target = &plan;
    (void)TranslateProgram(cap.code, pools, info, cap.Runtime(), profile);
    Native::TablePlan::lab_target = nullptr;
    const auto walker = info.srt_info.walker_func;
    const u32 size = info.srt_info.flattened_bufsize_dw;
    if (!walker || size <= NUM_USER_DATA_REGS) {
        std::printf("TABLES|no tables\n");
        return 0;
    }
    if (!plan.valid) {
        std::printf("TABLES|no plan;%s\n", plan.error.c_str());
        return 0;
    }
    if (plan.flat_dwords != size) {
        std::printf("TABLES|fail;plan has %u flattened dwords, the walker %u\n", plan.flat_dwords, size);
        return 0;
    }
    Shader::RegisterSrtFaultHandler();
    AddVectoredExceptionHandler(1, WalkerFault);
    // The plan as the pipeline cache stores it, read back: the runs below use the copy.
    {
        Serialization::Archive out;
        plan.Serialize(out);
        Serialization::Archive in{out.TakeOff()};
        Native::TablePlan copy;
        const auto same = [](const auto& a, const auto& b) {
            return a.size() == b.size() &&
                   (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0])) == 0);
        };
        if (!copy.Deserialize(in) || !copy.valid || copy.flat_dwords != plan.flat_dwords ||
            !same(copy.entries, plan.entries) || !same(copy.ops, plan.ops) ||
            !same(copy.table_dwords, plan.table_dwords)) {
            std::printf("TABLES|fail;the plan read back from its cache record differs\n");
            return 0;
        }
        plan = std::move(copy);
    }

    std::string failures;
    u32 checks = 0, walker_faults = 0, refused = 0, unmapped_faults = 0, unmapped_refused = 0;
    u32 replayed = 0; ///< captured nonzero dwords the walker read again
    // Both from the same user data; the comparison skips `skip` (dwords whose value the replay
    // replaced) when comparing with `expected`.
    const auto compare = [&](const char* what, const std::array<u32, NUM_USER_DATA_REGS>& user_data,
                             const std::vector<u32>* expected, const std::vector<bool>* skip) {
        std::vector<u32> by_walker(size, 0xCDCDCDCDu), by_plan(size, 0xCDCDCDCDu);
        std::memcpy(by_walker.data(), user_data.data(), sizeof(user_data));
        std::memcpy(by_plan.data(), user_data.data(), sizeof(user_data));
        const u32 faults = Shader::Info::srt_walk_faults;
        walker(user_data.data(), by_walker.data());
        walker_faults += Shader::Info::srt_walk_faults - faults;
        refused += plan.Run(user_data.data(), by_plan.data(), Readable);
        ++checks;
        for (u32 d = NUM_USER_DATA_REGS; d < size; ++d) {
            if (by_walker[d] != by_plan[d]) {
                u32 differ = 0;
                for (u32 e = NUM_USER_DATA_REGS; e < size; ++e) {
                    differ += by_walker[e] != by_plan[e];
                }
                char line[160];
                std::snprintf(line, sizeof(line), "%s: dword %u walker %#x plan %#x (%u of %u differ);",
                              what, d, by_walker[d], by_plan[d], differ, size - NUM_USER_DATA_REGS);
                failures += line;
                return;
            }
        }
        if (expected) {
            for (u32 d = NUM_USER_DATA_REGS; d < size && d < expected->size(); ++d) {
                if ((*skip)[d]) {
                    continue;
                }
                replayed += (*expected)[d] != 0;
                if (by_walker[d] != (*expected)[d]) {
                    char line[160];
                    std::snprintf(line, sizeof(line), "%s: dword %u walker %#x captured %#x;", what, d,
                                  by_walker[d], (*expected)[d]);
                    failures += line;
                    return;
                }
            }
        }
    };

    // 1. Null tables: every user data dword zero.
    compare("null", {}, nullptr, nullptr);

    // 2. Synthetic tables: a 64 MiB region below 4 GiB, every dword the address of one of its
    // 1024 64 KiB slots, so every pointer the walker reads (its low 48 bits) is in the region.
    static u32* region = nullptr;
    constexpr size_t RegionBytes = (64u << 20) + (1u << 20);
    for (uintptr_t at = 0x20000000; !region && at < 0xF0000000; at += 0x10000000) {
        region = static_cast<u32*>(VirtualAlloc(reinterpret_cast<void*>(at), RegionBytes,
                                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    }
    if (!region) {
        std::printf("TABLES|fail;no low memory for synthetic tables\n");
        return 0;
    }
    const u32 base = u32(reinterpret_cast<uintptr_t>(region));
    for (u32 seed = 1; seed <= 2; ++seed) {
        u32 x = seed * 0x9E3779B9u;
        for (size_t i = 0; i < RegionBytes / 4; ++i) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            region[i] = base + ((x % 1024u) << 16);
        }
        std::array<u32, NUM_USER_DATA_REGS> user_data{};
        for (u32 i = 0; i < user_data.size(); ++i) {
            user_data[i] = region[i * 977 + seed];
        }
        compare(seed == 1 ? "synthetic 1" : "synthetic 2", user_data, nullptr, nullptr);
    }

    // 3. Unmapped tables: every user data pointer aimed at reserved memory that is not committed
    // (above the null range): the walker faults on each read and its handler gives 0, the plan
    // refuses the reads. Both must give zeros: the case that skipped Ebrietas' draws.
    static void* unmapped = VirtualAlloc(nullptr, 1u << 20, MEM_RESERVE, PAGE_NOACCESS);
    if (unmapped) {
        const u64 address = reinterpret_cast<u64>(unmapped);
        std::array<u32, NUM_USER_DATA_REGS> user_data{};
        for (u32 i = 0; i + 1 < user_data.size(); i += 2) {
            user_data[i] = u32(address);
            user_data[i + 1] = u32(address >> 32);
        }
        const u32 faults_before = walker_faults, refused_before = refused;
        compare("unmapped", user_data, nullptr, nullptr);
        unmapped_faults = walker_faults - faults_before;
        unmapped_refused = refused - refused_before;
    }

    // 4. The captured tables: rebuilt from the plan and the flattened values the walker read in the
    // game, the walker must read them again. Pointers to tables below are replaced by the copies'
    // addresses (those dwords are not compared); a pointer the game had null stays null.
    if (cap.flat.size() == size) {
        const std::vector<u32>& flat = cap.flat;
        std::vector<bool> is_table(size), skip(size), nonzero_below(size);
        for (u32 i = 0; i < plan.entries.size(); ++i) {
            is_table[plan.entries[i].parent] = true;
        }
        for (u32 i = u32(plan.entries.size()); i-- > 0;) {
            const u32 d = Native::TablePlan::FirstFlat + i;
            const u16 parent = plan.entries[i].parent;
            if (flat[d] != 0 || nonzero_below[d]) {
                nonzero_below[parent] = true;
            }
        }
        // Offsets from the captured values (the plan's expressions read the flattened values).
        std::vector<u32> offsets(plan.entries.size());
        std::map<u16, u32> extent; // table -> dwords
        bool too_large = false;
        for (u32 i = 0; i < plan.entries.size(); ++i) {
            const auto& entry = plan.entries[i];
            u32 offset = entry.offset;
            if (entry.expr_len) {
                offset = plan.Offset(entry, flat.data());
            }
            offsets[i] = offset;
            if (offset > (1u << 20)) {
                too_large = true;
                break;
            }
            extent[entry.parent] = std::max(extent[entry.parent], offset + 2);
        }
        if (too_large) {
            failures += "replay skipped: offset beyond 4 MiB;";
        } else {
            std::map<u16, std::vector<u32>> tables;
            for (const auto& [table, dwords] : extent) {
                tables[table].assign(dwords, 0);
            }
            std::array<u32, NUM_USER_DATA_REGS> user_data = cap.user_data;
            const auto pointer_to = [&](u16 table) -> u64 {
                if (!nonzero_below[table]) {
                    return 0; // null in the game (or all zeros below): stays null
                }
                return reinterpret_cast<u64>(tables.at(table).data());
            };
            std::map<std::pair<u16, u32>, u32> placed;
            std::set<std::pair<u16, u32>> pointer_slots;
            bool conflict = false;
            // Pointers first: a value read at a pointer's high dword may come before it.
            for (u32 i = 0; i < plan.entries.size(); ++i) {
                const u32 d = Native::TablePlan::FirstFlat + i;
                const auto& entry = plan.entries[i];
                if (is_table[d]) {
                    auto& table = tables.at(entry.parent);
                    const u64 address = pointer_to(u16(d));
                    table[offsets[i]] = u32(address);
                    table[offsets[i] + 1] = u32(address >> 32);
                    pointer_slots.insert({entry.parent, offsets[i]});
                    pointer_slots.insert({entry.parent, offsets[i] + 1});
                    skip[d] = true;
                }
            }
            for (u32 i = 0; i < plan.entries.size(); ++i) {
                const u32 d = Native::TablePlan::FirstFlat + i;
                const auto& entry = plan.entries[i];
                auto& table = tables.at(entry.parent);
                const u32 offset = offsets[i];
                if (is_table[d]) {
                    continue;
                }
                if (pointer_slots.contains({entry.parent, offset})) {
                    skip[d] = true; // a table pointer's high dword, replaced
                    continue;
                }
                const auto [it, added] = placed.try_emplace({entry.parent, offset}, flat[d]);
                conflict |= !added && it->second != flat[d];
                table[offset] = it->second;
            }
            for (const auto& [table, dwords] : tables) {
                if (table < Native::TablePlan::FirstFlat) {
                    const u64 address = pointer_to(table);
                    user_data[table] = u32(address);
                    if (table + 1u < user_data.size()) {
                        user_data[table + 1] = u32(address >> 32);
                    }
                }
            }
            if (conflict) {
                failures += "replay: two flattened values at one table place differ;";
            }
            compare("captured", user_data, &flat, &skip);
        }
    }
    // The comparison must catch a wrong plan: one static offset moved by a dword, over the
    // synthetic tables of seed 2 (still in the region).
    const char* mutant = "none";
    for (size_t i = plan.entries.size(); i-- > 0;) {
        if (plan.entries[i].expr_len != 0) {
            continue;
        }
        Native::TablePlan wrong = plan;
        ++wrong.entries[i].offset;
        std::array<u32, NUM_USER_DATA_REGS> user_data{};
        for (u32 r = 0; r < user_data.size(); ++r) {
            user_data[r] = region[r * 977 + 2];
        }
        std::vector<u32> by_walker(size, 0), by_wrong(size, 0);
        std::memcpy(by_walker.data(), user_data.data(), sizeof(user_data));
        std::memcpy(by_wrong.data(), user_data.data(), sizeof(user_data));
        walker(user_data.data(), by_walker.data());
        wrong.Run(user_data.data(), by_wrong.data(), Readable);
        mutant = by_walker == by_wrong ? "missed" : "caught";
        break;
    }
    if (failures.empty()) {
        std::printf("TABLES|pass;checks=%u;walker_faults=%u;plan_refused=%u;unmapped_faults=%u;"
                    "unmapped_refused=%u;replayed=%u;mutant=%s\n",
                    checks, walker_faults, refused, unmapped_faults, unmapped_refused, replayed,
                    mutant);
    } else {
        std::printf("TABLES|fail;%s\n", failures.c_str());
    }
    return 0;
}

/// --census / --tables: every capture of `dir` in a child process, the lines aggregated.
int RunNative(const fs::path& self, const fs::path& dir, const fs::path& report, bool tables) {
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".bbs") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    std::ofstream csv(report);
    std::map<std::string, std::pair<u64, u32>> totals; ///< key -> sum, captures with it nonzero
    std::map<std::string, std::pair<u64, u32>> gcn;    ///< opcode -> count, captures using it
    std::map<std::string, u32> results, plan_errors;
    std::map<std::string, u64> counters;
    std::vector<std::string> keys;
    u32 crashed = 0, structured = 0, censused = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        DWORD code = 0;
        std::wstring args = std::wstring(tables ? L"--tables-one" : L"--census-one") + L" \"" +
                            files[i].wstring() + L"\"";
        if (g_native) args += L" --native";
        const std::string out = RunSelf(self, args, code);
        std::string census, ops, result, plan_error;
        for (size_t at = 0; at < out.size();) {
            const size_t end = out.find('\n', at);
            std::string row = out.substr(at, end == std::string::npos ? std::string::npos : end - at);
            if (!row.empty() && row.back() == '\r') row.pop_back();
            if (row.rfind("CENSUS|", 0) == 0) census = row.substr(7);
            if (row.rfind("GCN|", 0) == 0) ops = row.substr(4);
            if (row.rfind("TABLES|", 0) == 0) result = row.substr(7);
            if (row.rfind("PLAN|", 0) == 0) plan_error = row.substr(5);
            if (end == std::string::npos) break;
            at = end + 1;
        }
        const std::string name = files[i].filename().string();
        if (tables) {
            if (result.empty() || code != 0) {
                ++crashed;
                csv << name << ",crashed (exit " << code << ")\n";
                continue;
            }
            csv << name << ',' << result << '\n';
            ++results[result.substr(0, result.find(';'))];
            // The pass line's counters, summed over the captures.
            for (const char* key : {"walker_faults=", "plan_refused=", "unmapped_faults=",
                                    "unmapped_refused=", "replayed="}) {
                if (const size_t at = result.find(key); at != std::string::npos) {
                    counters[key] += std::strtoull(result.c_str() + at + std::strlen(key), nullptr, 10);
                }
            }
            if (result.find("mutant=caught") != std::string::npos) ++counters["wrong plans caught"];
            if (result.find("mutant=missed") != std::string::npos) ++counters["wrong plans missed"];
            if (result.rfind("no plan;", 0) == 0) ++plan_errors[result.substr(8)];
        } else {
            if (census.empty() || code != 0) {
                ++crashed;
                continue;
            }
            ++censused;
            if (!plan_error.empty()) ++plan_errors[plan_error];
            std::map<std::string, u64> row;
            for (size_t at = 0; at < census.size();) {
                const size_t end = census.find(';', at);
                const std::string pair = census.substr(at, end == std::string::npos ? std::string::npos : end - at);
                const size_t eq = pair.find('=');
                if (eq != std::string::npos) {
                    const std::string key = pair.substr(0, eq);
                    const u64 value = std::strtoull(pair.c_str() + eq + 1, nullptr, 10);
                    if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
                    row[key] = value;
                    totals[key].first += value;
                    totals[key].second += value != 0;
                }
                if (end == std::string::npos) break;
                at = end + 1;
            }
            structured += row["irreducible"] == 0 && row["multi_exit_loops"] == 0 && row["side_entries"] == 0;
            if (censused == 1) {
                csv << "file";
                for (const auto& key : keys) csv << ',' << key;
                csv << '\n';
            }
            csv << name;
            for (const auto& key : keys) csv << ',' << row[key];
            csv << '\n';
            for (size_t at = 0; at < ops.size();) {
                const size_t end = ops.find(';', at);
                const std::string pair = ops.substr(at, end == std::string::npos ? std::string::npos : end - at);
                const size_t eq = pair.find('=');
                if (eq != std::string::npos) {
                    auto& entry = gcn[pair.substr(0, eq)];
                    entry.first += std::strtoull(pair.c_str() + eq + 1, nullptr, 10);
                    ++entry.second;
                }
                if (end == std::string::npos) break;
                at = end + 1;
            }
        }
        if ((i + 1) % 200 == 0) std::printf("%zu of %zu\n", i + 1, files.size());
    }
    if (tables) {
        std::printf("Table plans: %zu captures, %u crashed\n", files.size(), crashed);
        for (const auto& [result, n] : results) std::printf("  %s: %u\n", result.c_str(), n);
        for (const auto& [key, n] : counters) {
            std::printf("  %s %llu\n", key.c_str(), (unsigned long long)n);
        }
    } else {
        std::ofstream out(fs::path(report).replace_filename(report.stem().string() + "-gcn.csv"));
        out << "opcode,count,captures\n";
        for (const auto& [name, entry] : gcn) out << name << ',' << entry.first << ',' << entry.second << '\n';
        std::printf("Census: %zu captures, %u read, %u crashed the current recompiler\n", files.size(),
                    censused, crashed);
        std::printf("  structured (reducible, one exit per loop, no side entries): %u\n", structured);
        for (const auto& key : keys) {
            std::printf("  %s: %llu in all, nonzero in %u\n", key.c_str(),
                        (unsigned long long)totals[key].first, totals[key].second);
        }
        std::printf("  GCN opcodes used: %zu\n", gcn.size());
    }
    for (const auto& [error, n] : plan_errors) std::printf("  no plan (%s): %u\n", error.c_str(), n);
    std::printf("Per shader: %s\n", report.string().c_str());
    return 0;
}
/// --curves: every capture's CurvesOne in a child process. The composites are listed; any other
/// shader the extension changed, or with curve channels that are not a composite, is a failure.
int RunCurves(const fs::path& self, const fs::path& dir, const fs::path& report) {
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".bbs") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    std::ofstream csv(report);
    csv << "file,result\n";
    u32 crashed = 0, composites = 0, failures = 0, with_channels = 0, varying = 0;
    for (const auto& file : files) {
        DWORD code = 0;
        std::wstring args = L"--curves-one \"" + file.wstring() + L"\"";
        if (g_native) {
            args += L" --native";
        }
        const std::string out = RunSelf(self, args, code);
        const size_t at = out.find("CURVES|");
        const std::string name = file.filename().string();
        if (at == std::string::npos || code != 0) {
            ++crashed;
            csv << name << ",crashed (exit " << code << ")\n";
            continue;
        }
        std::string line = out.substr(at + 7, out.find('\n', at) - at - 7);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        csv << name << ',' << line << '\n';
        const bool composite = line.find("composite=1") != std::string::npos;
        const bool changed = line.find("changed=1") != std::string::npos;
        const bool channels = line.rfind("channels=0", 0) != 0;
        with_channels += channels;
        if (composite) {
            ++composites;
            std::printf("  composite %s: %s\n", name.c_str(), line.c_str());
        }
        const bool varies = line.find("varies=1") != std::string::npos;
        if (varies) {
            ++varying;
            std::printf("  varies %s: the compiler gives other SPIR-V from run to run\n", name.c_str());
        }
        if ((composite != changed && !varies) || (!composite && channels)) {
            ++failures;
            std::printf("  FAIL %s: %s\n", name.c_str(), line.c_str());
        }
    }
    std::printf("Tone curves: %zu captures, %u crashed, %u composites recognised, %u shaders with "
                "curve channels, %u failures (%u compile differently from run to run)\n",
                files.size(), crashed, composites, with_channels, failures, varying);
    std::printf("Per shader: %s\n", report.string().c_str());
    return failures ? 1 : 0;
}
#endif

} // namespace

int main(int argc, char** argv) {
    // Options anywhere after the positional arguments.
    std::vector<char*> args;
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--native") == 0) {
            g_native = true;
        } else if (std::strcmp(argv[i], "--spv-dir") == 0 && i + 1 < argc) {
            g_spv_dir = argv[++i];
            fs::create_directories(g_spv_dir);
        } else {
            args.push_back(argv[i]);
        }
    }
    argc = int(args.size());
    argv = args.data();
#ifdef _WIN32
    if (argc >= 3 && (std::strcmp(argv[1], "--recompile-batch") == 0 || std::strcmp(argv[1], "--one") == 0 ||
                      std::strcmp(argv[1], "--walk-null") == 0 || std::strcmp(argv[1], "--census-one") == 0 ||
                      std::strcmp(argv[1], "--tables-one") == 0 ||
                      std::strcmp(argv[1], "--curves-one") == 0 || std::strcmp(argv[1], "--fix-keys") == 0)) {
        // A child the compiler ends (an assertion's breakpoint, a fault) is gone at once: Windows'
        // error reporting took seconds for each, over a minute for a cache with 17 of them.
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        SetUnhandledExceptionFilter([](EXCEPTION_POINTERS*) -> LONG {
            std::fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 0xC0DE);
            return EXCEPTION_EXECUTE_HANDLER;
        });
    }
#endif
    if (argc >= 3 && std::strcmp(argv[1], "--check-cache") == 0) {
        return CheckCache(argv[2]);
    }
    if (argc >= 3 && std::strcmp(argv[1], "--fix-keys") == 0) {
        return FixKeys(argv[2]);
    }
    if (argc >= 4 && std::strcmp(argv[1], "--export-catalog") == 0) {
        return ExportCatalog(argv[2], argv[3]);
    }
    if (argc >= 6 && std::strcmp(argv[1], "--seed-cache") == 0) {
#ifdef _WIN32
        wchar_t seed_self[MAX_PATH];
        GetModuleFileNameW(nullptr, seed_self, MAX_PATH);
        return SeedCache(seed_self, argv[2], argv[3], argv[4], argv[5]);
#else
        return SeedCache(argv[0], argv[2], argv[3], argv[4], argv[5]);
#endif
    }
    if (argc >= 4 && std::strcmp(argv[1], "--recompile-batch") == 0) {
        return RecompileBatch(argv[2], std::strtoull(argv[3], nullptr, 10), argc >= 5 ? fs::path(argv[4]) : fs::path{});
    }
    if (argc >= 3 && std::strcmp(argv[1], "--bench") == 0) {
        // The compiler books its phase times only then.
#ifdef _WIN32
        _putenv("BB_FRAME_STATS=1");
#else
        setenv("BB_FRAME_STATS", "1", 1);
#endif
        const fs::path dir = argv[2];
        return Bench(dir, argc >= 4 ? std::max(1, std::atoi(argv[3])) : 3,
                     argc >= 5 ? fs::path(argv[4]) : fs::path("lab-bench.csv"));
    }
    if (argc >= 3 && std::strcmp(argv[1], "--recompile-cache") == 0) {
#ifdef _WIN32
        wchar_t self_path[MAX_PATH];
        GetModuleFileNameW(nullptr, self_path, MAX_PATH);
        return RecompileCache(self_path, argv[2]);
#else
        return RecompileCache(argv[0], argv[2]);
#endif
    }
#ifdef _WIN32
    if (argc >= 3 && std::strcmp(argv[1], "--curves-one") == 0) {
        return CurvesOne(argv[2]);
    }
    if (argc >= 4 && std::strcmp(argv[1], "--ramp-modules") == 0) {
        return RampModules(argv[2], argv[3]);
    }
    if (argc >= 3 && std::strcmp(argv[1], "--ir") == 0) {
        return DumpIr(argv[2]);
    }
    if (argc >= 3 && std::strcmp(argv[1], "--census-one") == 0) {
        return CensusOne(argv[2]);
    }
    if (argc >= 3 && std::strcmp(argv[1], "--tables-one") == 0) {
        return TablesOne(argv[2]);
    }
    if (argc >= 3 && std::strcmp(argv[1], "--curves") == 0) {
        wchar_t curves_self[MAX_PATH];
        GetModuleFileNameW(nullptr, curves_self, MAX_PATH);
        const fs::path dir = argv[2];
        return RunCurves(curves_self, dir, argc >= 4 ? fs::path(argv[3]) : dir / "lab-curves.csv");
    }
    if (argc >= 3 && (std::strcmp(argv[1], "--census") == 0 || std::strcmp(argv[1], "--tables") == 0)) {
        wchar_t native_self[MAX_PATH];
        GetModuleFileNameW(nullptr, native_self, MAX_PATH);
        const fs::path dir = argv[2];
        const bool tables = std::strcmp(argv[1], "--tables") == 0;
        const fs::path report = argc >= 4 ? fs::path(argv[3]) : dir / (tables ? "lab-tables.csv" : "lab-census.csv");
        return RunNative(native_self, dir, report, tables);
    }
#endif
    if (argc >= 3 && std::strcmp(argv[1], "--walk-null") == 0) {
        return WalkNull(argv[2]);
    }
    if (argc >= 3 && std::strcmp(argv[1], "--one") == 0) {
        const fs::path file = argv[2];
        return RunOne(file, file.parent_path() / "profile.bin");
    }
    if (argc < 2) {
        std::fprintf(stderr, "usage: bb-shader-lab <capture folder> [report.csv]\n");
        return 2;
    }
    const fs::path dir = argv[1];
    const fs::path report = argc >= 3 ? fs::path(argv[2]) : dir / "lab-report.csv";
#ifdef _WIN32
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    return RunAll(self, dir, report);
#else
    return RunAll(argv[0], dir, report);
#endif
}
