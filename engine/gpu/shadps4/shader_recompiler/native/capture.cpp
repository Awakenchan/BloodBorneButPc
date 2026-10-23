// SPDX-License-Identifier: GPL-3.0-or-later
#include "shader_recompiler/native/capture.h"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/frontend/opcodes.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/cache_storage.h"

namespace Shader::Native {
namespace {

static_assert(std::is_trivially_copyable_v<RuntimeInfo>);
static_assert(std::is_trivially_copyable_v<Profile>);

/// BB_SHADER_CAPTURE_DIR, else "shaders" beside the session log (BB_LOG_FILE), else logs\shaders.
std::filesystem::path CaptureDir() {
    if (const char* dir = std::getenv("BB_SHADER_CAPTURE_DIR"); dir && dir[0]) {
        return dir;
    }
    if (const char* log = std::getenv("BB_LOG_FILE"); log && log[0]) {
        return std::filesystem::path(log).parent_path() / "shaders";
    }
    return std::filesystem::path("logs") / "shaders";
}

u64 Fnv(const void* data, size_t size, u64 h = 0xcbf29ce484222325ull) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        h = (h ^ p[i]) * 0x100000001b3ull;
    }
    return h;
}

/// The fetch shader's dwords, up to and including its s_setpc_b64 (at most 1024 dwords).
std::span<const u32> FetchCode(const Info& info) {
    if (!info.has_fetch_shader) {
        return {};
    }
    const auto ud = info.UserData();
    if (info.fetch_shader_sgpr_base + 1 >= ud.size()) {
        return {};
    }
    const u32* code;
    std::memcpy(&code, &ud[info.fetch_shader_sgpr_base], sizeof(code));
    if (!code) {
        return {};
    }
    Gcn::GcnCodeSlice slice(code, code + 1024);
    Gcn::GcnDecodeContext decoder;
    while (!slice.atEnd()) {
        const auto inst = decoder.decodeInstruction(slice);
        if (inst.opcode == Gcn::Opcode::S_SETPC_B64) {
            return {code, static_cast<size_t>(slice.position() - code)};
        }
    }
    return {};
}

/// What a compile reads besides the RuntimeInfo, copied out of guest memory. A draw-preparation
/// worker calls this under its fault guard (a fault jumps out of here): no lock may be held.
struct Collected {
    std::vector<u32> ud, words, fetch, flat;
    std::vector<CaptureSharp> sharps;
    CaptureTess tess{};
    std::vector<u8> tess_bytes;
    /// The Info fields the file header needs, and the geometry stage's copy shader (the
    /// RuntimeInfo only points to it): the file may be written after the compile is gone.
    u32 hw_stage = 0, sw_stage = 0, fetch_sgpr = ~0u;
    u64 hash = 0;
    std::vector<u32> vs_copy;
};

Collected Collect(const Info& info, const RuntimeInfo& runtime_info, std::span<const u32> code) {
    Collected c;
    c.hw_stage = u32(info.hw_stage);
    c.sw_stage = u32(info.sw_stage);
    c.hash = info.pgm_hash;
    c.fetch_sgpr = info.has_fetch_shader ? info.fetch_shader_sgpr_base : ~0u;
    if (info.hw_stage == HwStage::Geometry) {
        const auto& copy = runtime_info.hw.gs.vs_copy;
        c.vs_copy.assign(copy.begin(), copy.end());
    }
    const auto ud = info.UserData();
    c.ud.assign(ud.begin(), ud.end());
    c.words.assign(code.begin(), code.end());
    const auto fetch = FetchCode(info);
    c.fetch.assign(fetch.begin(), fetch.end());
    const auto flat = info.FlatUserData();
    c.flat.assign(flat.begin(), flat.end());
    // The vertex attributes' V#s: the compile reads each one's format and swizzle through a table
    // pointer in the user data (Gcn::VertexAttribute::GetSharp).
    if (!c.fetch.empty()) {
        if (const auto data = Gcn::ParseFetchShader(info)) {
            for (const auto& attrib : data->attributes) {
                if (attrib.sgpr_base == IR::NumScalarRegs) {
                    continue; // in the user data itself
                }
                const auto buffer = info.ReadUdReg<AmdGpu::Buffer>(attrib.sgpr_base, attrib.dword_offset);
                CaptureSharp sharp{attrib.sgpr_base, attrib.dword_offset, {}};
                static_assert(sizeof(buffer) == sizeof(sharp.words));
                std::memcpy(sharp.words, &buffer, sizeof(sharp.words));
                c.sharps.push_back(sharp);
            }
        }
    }
    // Hull and domain shaders read the tessellation constants while they compile
    // (Info::ReadTessConstantBuffer).
    if (info.tess_consts_dword_offset >= 0) {
        const auto sgpr = static_cast<u32>(info.tess_consts_ptr_base);
        const auto offset = static_cast<u32>(info.tess_consts_dword_offset);
        const auto buffer = info.ReadUdReg<AmdGpu::Buffer>(sgpr, offset);
        if (buffer.base_address) {
            c.tess.size = sizeof(TessellationDataConstantBuffer);
            c.tess.sgpr_base = sgpr;
            c.tess.dword_offset = offset;
            std::memcpy(c.tess.words, &buffer, sizeof(c.tess.words));
            const auto* bytes = reinterpret_cast<const u8*>(VAddr(buffer.base_address));
            c.tess_bytes.assign(bytes, bytes + c.tess.size);
        }
    }
    return c;
}

/// Written under a temporary name and renamed into place: a crash leaves no cut-short file.
bool WriteFile(const std::filesystem::path& path, const RuntimeInfo& runtime_info, const Collected& c) {
    auto temporary = path;
    temporary += ".tmp";
    FILE* f = std::fopen(temporary.string().c_str(), "wb");
    if (!f) {
        return false;
    }
    CaptureHeader header{};
    std::memcpy(header.magic, CaptureMagic, sizeof(header.magic));
    header.version = CaptureVersion;
    header.hw_stage = c.hw_stage;
    header.sw_stage = c.sw_stage;
    header.hash = c.hash;
    header.runtime_info_size = sizeof(RuntimeInfo);
    header.user_data_count = u32(c.ud.size());
    header.code_dwords = u32(c.words.size());
    header.fetch_dwords = u32(c.fetch.size());
    header.fetch_sgpr = c.fetch_sgpr;
    header.flat_dwords = u32(c.flat.size());
    bool ok = std::fwrite(&header, sizeof(header), 1, f) == 1 &&
              std::fwrite(&runtime_info, sizeof(RuntimeInfo), 1, f) == 1;
    const auto put = [&](const auto& v) {
        if (!v.empty()) {
            ok = ok && std::fwrite(v.data(), sizeof(v[0]), v.size(), f) == v.size();
        }
    };
    put(c.ud);
    put(c.words);
    put(c.fetch);
    put(c.flat);
    const u32 sharp_count = u32(c.sharps.size());
    ok = ok && std::fwrite(&sharp_count, sizeof(sharp_count), 1, f) == 1;
    put(c.sharps);
    ok = ok && std::fwrite(&c.tess, sizeof(c.tess), 1, f) == 1;
    put(c.tess_bytes);
    if (HwStage(c.hw_stage) == HwStage::Geometry && !c.vs_copy.empty()) {
        const auto& copy = c.vs_copy;
        const u32 trailer[2] = {u32(copy.size()), CopyTrailerMagic};
        ok = ok && std::fwrite(copy.data(), 4, copy.size(), f) == copy.size() &&
             std::fwrite(trailer, sizeof(trailer), 1, f) == 1;
    }
    ok = std::fclose(f) == 0 && ok;
    std::error_code error;
    if (ok) {
        std::filesystem::rename(temporary, path, error);
    }
    if (!ok || error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

/// bbport: the files are written on a thread of their own. They were written where the shader
/// compiles (the GPU thread, or a draw-preparation worker): a create, writes, close and rename in
/// the cache folder (the user's sits in a cloud-synced Desktop; the virus scanner reads each new
/// file at close) inside the compile time a new shader costs the frame. What a file holds is
/// collected at once (it is read from guest memory); only the writing waits.
/// BB_SHADER_FILE_ASYNC=0: written at once, as before.
bool AsyncFiles() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_SHADER_FILE_ASYNC");
        return !(env && env[0] == '0');
    }();
    return enabled;
}
class FileWriter {
public:
    static void Run(std::function<void()> job) {
        if (!AsyncFiles()) {
            job();
            return;
        }
        static FileWriter* writer = new FileWriter(); // never stopped: pending files at exit are lost
        {
            std::scoped_lock lk{writer->lock};
            writer->jobs.push_back(std::move(job));
        }
        writer->ready.notify_one();
    }

private:
    FileWriter() {
        std::thread([this] {
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock lk{lock};
                    ready.wait(lk, [this] { return !jobs.empty(); });
                    job = std::move(jobs.front());
                    jobs.pop_front();
                }
                job();
            }
        }).detach();
    }
    std::mutex lock;
    std::condition_variable ready;
    std::deque<std::function<void()>> jobs;
};

std::mutex mutex;
std::unordered_set<u64> written;
bool profile_written = false;
bool failed = false;
u32 count = 0;

std::mutex state_mutex;
std::unordered_set<u64> states_seen;
bool states_failed = false;
u32 states_written = 0;

} // namespace

bool CaptureEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_SHADER_CAPTURE");
        return env && env[0] == '1';
    }();
    return enabled;
}

void Capture(const Info& info, const RuntimeInfo& runtime_info, std::span<const u32> code,
             const Profile& profile) {
    if (!CaptureEnabled() || code.empty()) {
        return;
    }
    const u64 key = Fnv(&runtime_info, sizeof(runtime_info), info.pgm_hash * 0x9e3779b97f4a7c15ull);
    {
        std::scoped_lock lk{mutex};
        if (failed || !written.insert(key).second) {
            return;
        }
    }
    auto c = std::make_shared<const Collected>(Collect(info, runtime_info, code));
    char name[64];
    std::snprintf(name, sizeof(name), "%u_%016llx_%016llx.bbs", u32(info.hw_stage),
                  (unsigned long long)info.pgm_hash, (unsigned long long)key);
    FileWriter::Run([c, runtime_info, profile, file = std::string(name)] {
        std::scoped_lock lk{mutex};
        if (failed) {
            return;
        }
        const auto dir = CaptureDir();
        std::error_code error;
        std::filesystem::create_directories(dir, error);
        if (!profile_written) {
            profile_written = true;
            if (FILE* f = std::fopen((dir / "profile.bin").string().c_str(), "wb")) {
                const u32 size = sizeof(Profile);
                std::fwrite(&size, sizeof(size), 1, f);
                std::fwrite(&profile, sizeof(Profile), 1, f);
                std::fclose(f);
            }
        }
        if (!WriteFile(dir / file, runtime_info, *c)) {
            failed = true;
            std::printf("Shader capture: cannot write in %s; capture off\n", dir.string().c_str());
            return;
        }
        if (++count == 1 || count % 500 == 0) {
            std::printf("Shader capture: %u shaders written to %s\n", count, dir.string().c_str());
        }
    });
}

bool StatesEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_SHADER_STATES");
        return !(env && env[0] == '0');
    }();
    return enabled;
}

void StoreState(const Info& info, const RuntimeInfo& runtime_info, std::span<const u32> code,
                size_t perm_idx) {
    if (!StatesEnabled() || code.empty() || perm_idx >= 4096) {
        return;
    }
    {
        std::scoped_lock lk{state_mutex};
        if (states_failed || !states_seen.insert(info.pgm_hash * 4096 + perm_idx).second) {
            return;
        }
    }
    const auto dir = Storage::DataBase::Instance().FolderPath();
    if (dir.empty()) {
        return; // no cache, or the archive form of it
    }
    char name[64];
    std::snprintf(name, sizeof(name), "0x%016llx_%zu%s", (unsigned long long)info.pgm_hash,
                  perm_idx, StateExtension);
    const auto path = dir / name;
    const auto write = [path, dir, runtime_info](const Collected& c) {
        std::scoped_lock lk{state_mutex};
        if (states_failed) {
            return;
        }
        if (!WriteFile(path, runtime_info, c)) {
            states_failed = true;
            std::printf("Shader states: cannot write in %s; off for this session\n", dir.string().c_str());
            return;
        }
        if (++states_written == 1) {
            std::printf("Shader states: new shaders' compile inputs kept beside the cache, so a new "
                        "compiler rebuilds it before the game starts (BB_SHADER_STATES=0 turns it off)\n");
        }
    };
    if (!AsyncFiles()) {
        std::error_code error;
        if (std::filesystem::exists(path, error)) {
            return;
        }
        write(Collect(info, runtime_info, code));
        return;
    }
    // The existence check goes with the write: a state already stored costs a collect here
    // (copies of a few KiB) instead of a file system query on this thread.
    auto c = std::make_shared<const Collected>(Collect(info, runtime_info, code));
    FileWriter::Run([path, write, c] {
        std::error_code error;
        if (!std::filesystem::exists(path, error)) {
            write(*c);
        }
    });
}

} // namespace Shader::Native
