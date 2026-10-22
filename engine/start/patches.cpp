// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from scripts/patches.py: the comments there explain each correction; this keeps them
// short and the behaviour, byte for byte, the same.
#include "patches.h"
#include "elf.h"
#include "xml.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>

namespace start {
namespace {

constexpr std::uint64_t kBase = 0x400000;
const Size kOutput{1920, 1080};
const double kPresetScales[] = {1.0, 1.5, 1.7, 2.0, 3.0};
const char* const kResolutionTemplate = "Resolution Patch 1280x720 (16:9)";
const char* const kScaledBase = "90 FPS++";
const char* const kSprint = "Sprint Fix (High FPS)";
const char* const kIntelTonemap = "Intel Black Tonemap Fix";
const char* const kSfxWorkaround = "Intel 12th Gen+ SFX workaround";
const char* const kHeapPatch = "Increased Graphics Heap Sizes";
constexpr std::uint64_t kHeapSizes = 0x4736dd8;  // image offset: the two graphics heap sizes
// The game's GX range allocator (scripts/patches.py GX_REGION_*): the GX manager's default 128 MiB
// (mov dword [rbx+0x88], 0x8000000) plus 32 MiB, carved at start from the graphics heaps.
constexpr std::uint64_t kGxRegion = 0x1d9933d;
constexpr std::uint64_t kGxHeapPs4 = 0x8ef00000;  // GFX_GraphicsPrivate at the PS4 size (kHeapSizes)
constexpr unsigned kGxRegionGame = 160, kGxRegionDefault = 288, kGxRegionMax = 416;  // MiB

using Write = std::pair<std::uint64_t, Bytes>;  // image offset, bytes
using Writes = std::vector<Write>;

const std::map<std::string, std::vector<std::string>>& FpsPresets() {
    static const std::map<std::string, std::vector<std::string>> presets{
        {"30", {}}, {"60", {"60 FPS++"}}, {"90", {"90 FPS++", kSprint}}, {"uncap", {"Uncap FPS++", kSprint}}};
    return presets;
}

const std::vector<std::tuple<std::string, const char*, const char*>>& Effects() {
    static const std::vector<std::tuple<std::string, const char*, const char*>> effects{
        {"effect_chromatic_aberration", "Disable Chromatic Aberration", nullptr},
        {"effect_dof", "Disable DoF", nullptr},
        {"effect_motion_blur", "Disable Motion Blur (perf increase)", nullptr},
        {"effect_ssao", "Disable SSAO", nullptr},
        {"effect_game_aa", "Disable AA", nullptr},
        {"effect_dynamic_shadows", "Disable Dynamic Light Shadows (perf increase)", nullptr},
        {"effect_ssr", nullptr, "Enable Screen Space Reflections (READ NOTE)"},
        {"skip_intro", nullptr, "Skip Intro"},
        {"debug_camera", nullptr, "Restore Debug Camera"},
        {"debug_menu", nullptr, "Restore Debug Menu (READ NOTES)"},
    };
    return effects;
}
const std::map<std::string, std::string> kModelLod{
    {"-2", "Model LOD -2 (Highest)"}, {"1", "Model LOD 1 (Lower)"}, {"2", "Model LOD 2 (Lowest)"}};
const std::set<std::string> kDefaultOff{"effect_chromatic_aberration", "effect_dof", "effect_motion_blur", "effect_game_aa"};
const std::set<std::string> kBloodborneIds{"CUSA00207", "CUSA00208", "CUSA00299", "CUSA00900", "CUSA01363",
                                           "CUSA03014", "CUSA03023", "CUSA03173", "CUSA03179"};

Bytes F32(double value) {
    Bytes out(4);
    Store<float>(out.data(), float(value));
    return out;
}
Bytes Concat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}
template <typename T> Bytes Le(T value) {
    Bytes out(sizeof value);
    Store<T>(out.data(), value);
    return out;
}

/// Python's repr of a list of strings, for the log line patches.py printed.
std::string Repr(const std::vector<std::string>& names) {
    std::string out = "[";
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::string& n = names[i];
        const char quote = (n.find('\'') != std::string::npos && n.find('"') == std::string::npos) ? '"' : '\'';
        out += (i ? ", " : "") + std::string(1, quote);
        for (char c : n) {
            if (c == '\\' || c == quote) out += '\\';
            out += c;
        }
        out += quote;
    }
    return out + "]";
}

/// int(text, 0) for the forms patch files use: 0x hex, plain decimal.
std::optional<std::int64_t> ParseInt(std::string_view text, bool allow_negative = false) {
    std::string t = Trim(text);
    std::erase(t, '_');
    bool negative = false;
    if (!t.empty() && (t[0] == '+' || t[0] == '-')) {
        negative = t[0] == '-';
        t.erase(0, 1);
    }
    if (t.empty()) return std::nullopt;
    int base = 10;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) base = 16, t.erase(0, 2);
    else if (t.size() > 2 && t[0] == '0' && (t[1] == 'o' || t[1] == 'O')) base = 8, t.erase(0, 2);
    else if (t.size() > 2 && t[0] == '0' && (t[1] == 'b' || t[1] == 'B')) base = 2, t.erase(0, 2);
    else if (t.size() > 1 && t[0] == '0' && t.find_first_not_of('0') != std::string::npos) return std::nullopt;
    std::uint64_t value = 0;
    for (char c : t) {
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
        if (d >= base) return std::nullopt;
        value = value * std::uint64_t(base) + std::uint64_t(d);
    }
    if (negative && !allow_negative) return std::nullopt;
    return negative ? -std::int64_t(value) : std::int64_t(value);
}

Bytes Encode(const XmlElement& line) {
    const std::string kind = line.Get("Type", ""), value = line.Get("Value", "");
    if (kind == "bytes") return FromHex(value);
    if (kind == "bytes16" || kind == "bytes32" || kind == "bytes64") {
        const auto number = ParseInt(value);
        if (!number) throw Error("invalid literal for int(): '" + value + "'");
        const int n = std::stoi(kind.substr(5)) / 8;
        if (n < 8 && std::uint64_t(*number) >> (n * 8)) throw Error("int too big to convert");
        Bytes out(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) out[std::size_t(i)] = std::uint8_t(std::uint64_t(*number) >> (8 * i));
        return out;
    }
    if (kind == "float32" || kind == "float64") {
        char* end = nullptr;
        const std::string t = Trim(value);
        const double v = std::strtod(t.c_str(), &end);
        if (t.empty() || *end) throw Error("could not convert string to float: '" + value + "'");
        if (kind == "float32") return F32(v);
        return Le<double>(v);
    }
    if (kind == "utf8") {
        Bytes out(value.begin(), value.end());
        out.push_back(0);
        return out;
    }
    if (kind == "utf16") {
        const std::wstring w = Wide(value);
        Bytes out;
        for (wchar_t c : w) Append<std::uint16_t>(out, std::uint16_t(c));
        out.push_back(0);
        out.push_back(0);
        return out;
    }
    throw Error("unsupported patch type '" + kind + "'");
}

struct Segments {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    bool Contains(std::uint64_t offset, std::size_t size) const {
        return std::any_of(ranges.begin(), ranges.end(),
                           [&](const auto& r) { return r.first <= offset && offset + size <= r.second; });
    }
};

Segments EbootSegments(const Bytes& elf) {
    Segments s;
    for (const Ph& p : ProgramHeaders(elf))
        if (p.type == 1) s.ranges.emplace_back(p.vaddr, p.vaddr + p.memsz);
    return s;
}

class PatchFile {
public:
    explicit PatchFile(const fs::path& path) : path_(path), root_(ParseXml(path)) {}
    const fs::path& FilePath() const { return path_; }
    const XmlElement& Root() const { return *root_; }

    Writes Compile(const std::vector<std::string>& names, const std::string& app_version, const Segments& segments) const {
        std::map<std::string, const XmlElement*> found;
        for (const XmlElement* meta : root_->Iter("Metadata")) {
            const auto name = meta->Get("Name");
            if (name && std::find(names.begin(), names.end(), *name) != names.end() &&
                meta->Get("AppVer") == app_version && meta->Get("AppElf", "eboot.bin") == "eboot.bin")
                found[*name] = meta;
        }
        std::vector<std::string> missing;
        for (const std::string& n : names)
            if (!found.count(n)) missing.push_back(n);
        if (!missing.empty()) throw Error("patches not found for app version " + app_version + ": " + Repr(missing));
        Writes writes;
        for (const std::string& name : names) {
            for (const XmlElement* line : found[name]->Iter("Line")) {
                const std::string address = line->Get("Address", "");
                const auto value = ParseInt(address);
                if (!value) throw Error(name + ": invalid literal for int(): '" + address + "'");
                const std::uint64_t offset = std::uint64_t(*value) - kBase;
                Bytes data = Encode(*line);
                if (!segments.Contains(offset, data.size()))
                    throw Error(name + ": address " + address + " is outside the eboot");
                writes.emplace_back(offset, std::move(data));
            }
        }
        return writes;
    }

private:
    fs::path path_;
    std::unique_ptr<XmlElement> root_;
};

// --- frame-rate values and corrections --------------------------------------------------------

std::map<std::uint64_t, Bytes> ScaledRateValues(int fps) {
    const Bytes frame = Concat({FromHex("41c7442418"), F32(1.0 / fps)});
    const std::uint32_t cap = fps == 90 ? 10000 : fps == 120 ? 8000 : 6500;
    Bytes four = Concat({F32(3.0 / fps), F32(3.0 / fps), F32(3.0 / fps), F32(3.0 / fps)});
    return {
        {0x0F383CA, F32(1.0 / fps)},
        {0x1CBDB42, Concat({FromHex("c7857cffffff"), F32(0.0125 * 90 / fps), FromHex("909090909090")})},
        {0x1929676, Le<std::int32_t>(fps / 6)},
        {0x192C405, Le<std::int32_t>(fps / 3)},
        {0x2434840, frame}, {0x243485A, frame}, {0x243487E, frame}, {0x24348B3, frame},
        {0x25629F0, Concat({Bytes{0xbf}, Le<std::uint32_t>(990000 / fps)})},
        {0x2C012CC, Concat({Bytes{0x81, 0xff}, Le(cap), Bytes{0x7e, 0x05, 0xbf}, Le(cap), FromHex("e98aeb3b00")})},
        {0x4CF6420, four},
        {0x4D27FD4, F32(-3.0 / fps)}, {0x4D27FE0, F32(3.0 / fps)},
        {0x4D29170, F32(1.0 / fps)},
    };
}

// The per-frame steps 60 FPS++ halves from the game's 1/30 to 1/60 (physics, falling objects and
// more): the 90 FPS++ base keeps them at 1/60, so at 120 FPS they ran twice as fast ("gravity
// doubled"). Here they are 1/fps. 0x4D27C4C is the effects' second step (#15), fixed separately.
// BB_FRAME_STEP_FIX=0 keeps them at 1/60.
constexpr std::uint64_t kEffectSecondStep = 0x4D27C4C;

Writes RateScaled(const Writes& writes, int fps) {
    auto values = ScaledRateValues(fps);
    const Bytes sixtieth = F32(1.0 / 60), step = F32(1.0 / fps);
    const bool step_fix = Env("BB_FRAME_STEP_FIX", "1") != "0";
    Writes out;
    std::set<std::uint64_t> seen;
    for (const auto& [offset, data] : writes) {
        const auto it = values.find(offset + kBase);
        if (step_fix && it == values.end() && data == sixtieth && offset + kBase != kEffectSecondStep) {
            out.emplace_back(offset, step);
            continue;
        }
        if (it != values.end()) {
            if (it->second.size() != data.size())
                throw Error(std::string(kScaledBase) + ": " + Hex(offset + kBase) + " changed size");
            out.emplace_back(offset, it->second);
            seen.insert(offset + kBase);
        } else {
            out.emplace_back(offset, data);
        }
    }
    std::string missing;
    for (const auto& [address, _] : values)
        if (!seen.count(address)) missing += (missing.empty() ? "" : ", ") + Hex(address);
    if (!missing.empty()) throw Error(std::string(kScaledBase) + ": no write at " + missing);
    return out;
}

const std::set<std::uint64_t> kEffectRateSites{0x26FB39F, 0x4CAD7FC};
constexpr std::uint64_t kClothStiffness = 0x4D3BD00;
const std::pair<std::uint64_t, std::uint64_t> kClothLeftOut[] = {
    {0xB6FBD4, 0xB6FBD8}, {0xB73F84, 0xB73F8D}, {0xB73FA8, 0xB73FC1}, {0x4D3BD10, 0x4D3BD20}};
const std::map<std::uint64_t, std::uint32_t> kCameraFields{
    {0x183a4c3, 0x19c}, {0x183a4cd, 0x1a0}, {0x183a4d7, 0x1a4}, {0x183a4e1, 0x1a8}, {0x183a4f5, 0x1b0},
    {0x183a4ff, 0x1b4}, {0x183a509, 0x1b8}, {0x183a527, 0x1c4}, {0x183a531, 0x1c8}, {0x183a53b, 0x1cc},
    {0x183a649, 0x238}, {0x183a653, 0x23c}, {0x183a65d, 0x240}, {0x183a8c1, 0x19c}, {0x183a8cb, 0x1a0},
    {0x183a8d5, 0x1a4}, {0x183a8df, 0x1a8}, {0x183a8f3, 0x1b0}, {0x183a8fd, 0x1b4}, {0x183a907, 0x1b8},
    {0x183a925, 0x1c4}, {0x183a92f, 0x1c8}, {0x183a939, 0x1cc}, {0x183aa47, 0x238}, {0x183aa51, 0x23c},
    {0x183aa5b, 0x240}};
// The stores and the jump after each block that skips the game's parameter reads (0x183FF6B,
// 0x18407E6): ends exclusive. Before 0.9.34 beta 5 the ranges ended at the jumps, so Original kept
// them: the per-area camera parameters still never loaded, and the second jump, written into the
// middle of the game's lea at 0x18407E3, turned the next instruction into another one.
const std::pair<std::uint64_t, std::uint64_t> kCameraStores[] = {{0x183FF29, 0x183FF70}, {0x1840799, 0x18407EB}};
constexpr std::uint32_t kCameraAngle = 0x238, kCameraKept = 0x1a8;
const std::map<std::uint64_t, double> kCameraEases{{0x4D25E68, 0.1}, {0x4D25E94, 1.0 / 30}};
constexpr std::uint64_t kCameraParamEase = 0x183A7DE;
const Bytes kCameraParamWas = {0xcd, 0xcc, 0x4c, 0x3d};
constexpr std::uint64_t kEffectStepFirst = 0x1CBC4FB, kEffectStepSecond = 0x1CBC511;
const Bytes kEffectStepWas = {0x37, 0xb7, 0x06, 0x03}, kEffectStepNow = {0xf3, 0xe3, 0xa9, 0x03};

double CameraValue(std::optional<std::uint32_t> field, double value, double frames) {
    if (field == kCameraAngle || !(0 < value && value < 1)) return value;
    return field == kCameraKept ? std::pow(value, frames) : 1 - std::pow(1 - value, frames);
}

// The follow camera. The FPS++ lists carry Kyo's retune of it: the constructor's defaults
// (kCameraFields, a 40 degree pitch angle among them), the parameter reads replaced by stores
// of fixed values (kCameraStores: the game's per-area camera parameters never load) and the
// wall escape at 0x183DCFE; players saw a camera that pulled back, lagged and widened while
// sprinting (GitHub issue 32). Original (the default; camera_tuning=fps++ or
// BB_CAMERA_TUNING=fps++ keeps the retune) leaves all of them as the game has them and
// converts the game's own per-frame eases, distance (0.1) and chase rate (1/30), from a 30 FPS
// frame: the camera of the game at 30 at every rate, as bbhost's Camera tuning: Original.
constexpr std::uint64_t kCameraWallEscape = 0x183DCFE;
// The jump the retune puts over 1.5 KiB of the game's follow camera (to 0x183E271), with the wall
// escape inside the code it skips.
constexpr std::uint64_t kCameraSkip = 0x183DC96;

bool KyoCameraSite(std::uint64_t a, std::size_t size) {
    if (kCameraFields.count(a) || a == kCameraWallEscape || a == kCameraSkip) return true;
    return std::any_of(std::begin(kCameraStores), std::end(kCameraStores),
                       [&](const auto& r) { return a < r.second && a + size > r.first; });
}

Writes FrameRateFixes(const Writes& writes, std::optional<int> fps, const Bytes& elf, bool original_camera) {
    const bool camera = !original_camera && Env("BB_CAMERA_FIX", "0") == "1";
    original_camera = original_camera && fps.has_value();
    Writes out;
    for (const auto& [offset, original] : writes) {
        Bytes data = original;
        const std::uint64_t a = offset + kBase;
        if (kEffectRateSites.count(a)) continue;
        if (original_camera) {
            if (KyoCameraSite(a, data.size())) continue;
            if (const auto ease = kCameraEases.find(a); ease != kCameraEases.end()) {
                out.emplace_back(offset, F32(CameraValue(std::nullopt, ease->second, 30.0 / *fps)));
                continue;
            }
        }
        if (fps && *fps > 60) {
            bool skip = false;
            for (const auto& [lo, hi] : kClothLeftOut) {
                if (lo <= a && a < hi) {
                    if (a + data.size() > hi) throw Error("frame-rate patch: " + Hex(a) + " reaches past " + Hex(hi));
                    skip = true;
                    break;
                }
            }
            if (skip) continue;
            if (a == kClothStiffness) {
                const Bytes one = F32(std::pow(60.0 / *fps, 2));
                data = Concat({one, one, one, one});
            }
        }
        if (fps && camera) {
            if (const auto field = kCameraFields.find(a); field != kCameraFields.end()) {
                data = F32(CameraValue(field->second, Load<float>(data.data()), 60.0 / *fps));
            } else if (std::any_of(std::begin(kCameraStores), std::end(kCameraStores),
                                   [&](const auto& r) { return r.first <= a && a < r.second; }) &&
                       data.size() == 11 && data[0] == 0x41 && data[1] == 0xc7 && data[2] == 0x85) {
                const std::uint32_t f = Load<std::uint32_t>(data.data() + 3);
                const float value = Load<float>(data.data() + 7);
                const Bytes v = F32(CameraValue(f, value, 60.0 / *fps));
                std::copy(v.begin(), v.end(), data.begin() + 7);
            } else if (const auto ease = kCameraEases.find(a); ease != kCameraEases.end()) {
                data = F32(CameraValue(std::nullopt, ease->second, 30.0 / *fps));
            }
        }
        out.emplace_back(offset, std::move(data));
    }
    if (std::any_of(out.begin(), out.end(), [](const Write& w) { return w.first + kBase == kEffectStepFirst; }) &&
        OriginalBytes(elf, kEffectStepSecond - kBase, kEffectStepWas.size()) == kEffectStepWas)
        out.emplace_back(kEffectStepSecond - kBase, kEffectStepNow);
    if (fps && camera && OriginalBytes(elf, kCameraParamEase - kBase, 4) == kCameraParamWas)
        out.emplace_back(kCameraParamEase - kBase,
                         F32(CameraValue(std::nullopt, Load<float>(kCameraParamWas.data()), 30.0 / *fps)));
    return out;
}

// --- sizes ------------------------------------------------------------------------------------

int Round(double v) {
    return int(std::nearbyint(v));  // round half to even, as Python's round()
}

double PresetScale(const std::map<std::string, std::string>& settings) {
    int preset = 0;
    try {
        const std::string text = Setting(settings, "preset").value_or("0");
        preset = text.empty() ? 0 : std::stoi(text);
    } catch (const std::exception&) {
        throw Error("invalid preset");
    }
    // bbport: preset 5 (Custom): render_scale, 1.0-3.0 (BbSettings::PresetScale).
    if (preset == int(std::size(kPresetScales))) {
        double scale = 1.3;
        try {
            const std::string value = Setting(settings, "render_scale").value_or("1.3");
            scale = value.empty() ? 1.3 : std::stod(value);
        } catch (const std::exception&) {
            scale = 1.3;
        }
        return std::clamp(std::isfinite(scale) ? scale : 1.3, 1.0, 3.0);
    }
    return kPresetScales[std::clamp(preset, 0, int(std::size(kPresetScales)) - 1)];
}

std::optional<Size> RenderSize(const std::map<std::string, std::string>& settings, const std::string& override_size) {
    if (!override_size.empty()) {
        const auto size = ParseSize(override_size);
        if (!size) throw Error("invalid render resolution " + override_size);
        return size;
    }
    if (Setting(settings, "upscaler").value_or("fsr3") == "off") return std::nullopt;
    const double scale = PresetScale(settings);
    if (scale == 1.0) return std::nullopt;
    // Multiples of 4, as ScaledSizes: the game pads its targets to them (undrawn edge columns).
    return Size{std::max(4, Round(kOutput.w / scale / 4) * 4), std::max(4, Round(kOutput.h / scale / 4) * 4)};
}

Size OutputSize(const std::map<std::string, std::string>& settings) {
    std::string value = Lower(Setting(settings, "output_res").value_or(""));
    if (value == "auto") value = Env("BB_DISPLAY_SIZE", "");
    if (const auto size = ParseSize(value); size && size->w > 0 && size->h > 0) return *size;
    return kOutput;
}

Size UiSize(const std::map<std::string, std::string>& settings, Size out) {
    Size ui = kOutput;
    if (Setting(settings, "widescreen").value_or("1") == "0" || std::int64_t(out.w) * kOutput.h == std::int64_t(out.h) * kOutput.w)
        ui = kOutput;
    else if (std::int64_t(out.w) * kOutput.h > std::int64_t(out.h) * kOutput.w)
        ui = {std::min(8192, Round(double(kOutput.h) * out.w / out.h / 2) * 2), kOutput.h};
    else
        ui = {kOutput.w, std::min(8192, Round(double(kOutput.w) * out.h / out.w / 2) * 2)};
    // Sharp UI (BB_SHARP_UI=1): the movie viewport at the output's size, so Scaleform draws its
    // text, shapes and pictures for that many pixels (the 1920x1080 stage scaled to fill it)
    // instead of for 1080 lines that are stretched afterwards. Never below the 1080p viewport.
    if (Env("BB_SHARP_UI", "") == "1") {
        const double k = std::min(double(out.w) / ui.w, double(out.h) / ui.h);
        if (k > 1.0) ui = {std::min(8192, Round(ui.w * k / 2) * 2), std::min(8192, Round(ui.h * k / 2) * 2)};
    }
    return ui;
}

// --- selection --------------------------------------------------------------------------------

std::vector<std::string> EffectPatches(const std::map<std::string, std::string>& settings) {
    std::vector<std::string> names;
    for (const auto& [key, off, on] : Effects()) {
        const auto value = Setting(settings, key);
        if (!value) {
            if (kDefaultOff.count(key) && off) names.push_back(off);
            continue;
        }
        const char* name = *value == "1" ? on : off;
        if (name) names.push_back(name);
    }
    if (const auto lod = kModelLod.find(Setting(settings, "model_lod").value_or("0")); lod != kModelLod.end())
        names.push_back(lod->second);
    return names;
}

bool ManagedBuiltIn(const std::string& name) {
    for (const auto& [_, names] : FpsPresets())
        if (std::find(names.begin(), names.end(), name) != names.end()) return true;
    for (const char* n : {kScaledBase, "30 FPS++", "60FPS (no deltatime)", kIntelTonemap, kSfxWorkaround, kHeapPatch})
        if (name == n) return true;
    for (const auto& [_, off, on] : Effects())
        if ((off && name == off) || (on && name == on)) return true;
    for (const auto& [_, n] : kModelLod)
        if (name == n) return true;
    return StartsWith(name, "Resolution Patch");
}

nlohmann::json ReadConfig(const fs::path& config) {
    std::error_code error;
    if (config.empty() || !fs::is_regular_file(config, error)) return nlohmann::json::object();
    return nlohmann::json::parse(ReadText(config));
}

std::vector<std::string> BuiltinSelection(const PatchFile& xml, const fs::path& config) {
    std::vector<std::string> enabled;
    try {
        const auto settings = ReadConfig(config);
        if (settings.contains("enabled"))
            for (const auto& key : settings.at("enabled"))
                if (key.is_string()) enabled.push_back(key.get<std::string>());
    } catch (const std::exception&) {
        return {};
    }
    const std::string prefix = Utf8(xml.FilePath().filename()) + "/";
    std::set<std::string> known;
    for (const XmlElement* meta : xml.Root().Iter("Metadata"))
        if (const auto name = meta->Get("Name")) known.insert(*name);
    std::vector<std::string> out;
    for (const std::string& key : enabled) {
        if (!StartsWith(key, prefix)) continue;
        const std::string name = key.substr(prefix.size());
        if (known.count(name) && !ManagedBuiltIn(name)) out.push_back(name);
    }
    return out;
}

bool BreaksOnline(const Writes& writes) {
    constexpr std::uint64_t start = 0x1fb9190, end = 0x1fb91e3;
    if (Env("BB_ONLINE", "") != "1" || Env("BB_REGION_PATCH_ONLINE", "") == "keep") return false;
    return std::any_of(writes.begin(), writes.end(),
                       [](const Write& w) { return w.first < end && start < w.first + w.second.size(); });
}

void SkipForOnline(const std::string& key) {
    Log("Patches: skipped %s while online is on: it moves the game to a region with no Bloodborne server (Play "
        "online would fail); it applies again with online off",
        key.c_str());
}

std::vector<std::string> CheckedPatches(const PatchFile& xml, const std::vector<std::string>& names,
                                        const std::string& app_version, const Segments& segments, const Bytes& elf,
                                        const std::vector<std::string>& group) {
    nlohmann::json table;
    try {
        table = nlohmann::json::parse(ReadText(xml.FilePath().parent_path() / "Bloodborne-1.09-sites.json"));
    } catch (const std::exception&) {
        return names;
    }
    std::map<std::string, std::vector<std::uint64_t>> bad;
    std::vector<std::string> bad_order;
    for (const std::string& name : names) {
        if (!table.contains(name)) continue;
        const Writes writes = xml.Compile({name}, app_version, segments);
        const auto& expected = table.at(name);
        std::vector<std::uint64_t> diff;
        if (expected.size() != writes.size()) {
            for (const auto& w : writes) diff.push_back(w.first);
        } else {
            for (std::size_t i = 0; i < writes.size(); ++i) {
                const auto original = OriginalBytes(elf, writes[i].first, writes[i].second.size());
                const std::uint32_t crc = original ? Crc32(original->data(), original->size()) : Crc32(nullptr, 0);
                if (crc != expected[i].get<std::uint32_t>()) diff.push_back(writes[i].first);
            }
        }
        if (!diff.empty() && !bad.count(name)) bad_order.push_back(name);
        if (!diff.empty()) bad[name] = diff;
    }
    if (std::any_of(group.begin(), group.end(), [&](const std::string& n) { return bad.count(n); }))
        for (const std::string& name : group)
            if (std::find(names.begin(), names.end(), name) != names.end() && !bad.count(name)) {
                bad[name] = {};
                bad_order.push_back(name);
            }
    for (const std::string& name : bad_order) {
        const auto& diff = bad[name];
        const std::string where =
            !diff.empty() ? "at " + std::to_string(diff.size()) + " of its " + std::to_string(table.at(name).size()) +
                                " spots (first: image offset " + Hex(diff[0]) + ")"
                          : "with the FPS patch above";
        Log("Patches: \"%s\" left out: this game executable is not the standard 1.09 %s", name.c_str(), where.c_str());
    }
    std::vector<std::string> out;
    for (const std::string& name : names)
        if (!bad.count(name)) out.push_back(name);
    return out;
}

bool SameAppVersion(const std::optional<std::string>& value, const std::string& app_version) {
    if (!value) return false;
    const auto parse = [](const std::string& text) -> std::optional<std::vector<int>> {
        std::vector<int> parts;
        for (const std::string& p : Split(Trim(text), '.')) {
            const auto v = ParseInt(p);
            if (!v && Trim(p).find_first_not_of('0') == std::string::npos && !Trim(p).empty()) {
                parts.push_back(0);
                continue;
            }
            if (!v) {
                // int('09') is 9 in Python: leading zeros are fine for int(text) without a base.
                const std::string t = Trim(p);
                if (t.empty() || t.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
                parts.push_back(std::stoi(t));
                continue;
            }
            parts.push_back(int(*v));
        }
        return parts;
    };
    const auto a = parse(*value), b = parse(app_version);
    return a && b && *a == *b;
}

struct External {
    std::string key;
    const XmlElement* meta;
};

void ExternalPatches(const fs::path& directory, const std::string& app_version, const fs::path& builtin,
                     std::vector<std::unique_ptr<PatchFile>>& files, std::vector<External>& found) {
    std::error_code error;
    if (!fs::is_directory(directory, error)) return;
    std::vector<fs::path> paths;
    for (const auto& entry : fs::directory_iterator(directory, error))
        if (entry.is_regular_file(error) && Lower(Utf8(entry.path().extension())) == ".xml") paths.push_back(entry.path());
    std::sort(paths.begin(), paths.end(), [](const fs::path& a, const fs::path& b) {
        return Lower(Utf8(a.filename())) < Lower(Utf8(b.filename()));
    });
    const std::string builtin_name = Lower(Utf8(builtin.filename()));
    for (const fs::path& path : paths) {
        if (fs::equivalent(path, builtin, error) || Lower(Utf8(path.filename())) == builtin_name) continue;
        std::unique_ptr<PatchFile> file;
        try {
            file = std::make_unique<PatchFile>(path);
        } catch (const Error& e) {
            Log("Patches: %s: %s", Utf8(path.filename()).c_str(), e.what());
            continue;
        }
        std::set<std::string> ids;
        for (const XmlElement* id : file->Root().Iter("ID"))
            if (!id->text.empty()) ids.insert(Trim(id->text));
        if (!ids.empty() && std::none_of(ids.begin(), ids.end(), [](const std::string& i) { return kBloodborneIds.count(i); }))
            continue;
        for (const XmlElement* meta : file->Root().Iter("Metadata"))
            if (SameAppVersion(meta->Get("AppVer"), app_version) && meta->Get("AppElf", "eboot.bin") == "eboot.bin")
                found.push_back({Utf8(path.filename()) + "/" + meta->Get("Name", "None"), meta});
        files.push_back(std::move(file));
    }
}

Writes CompileExternal(const std::vector<External>& selected, const Segments& segments,
                       const std::optional<std::pair<std::set<std::uint64_t>, Size>>& resolution) {
    Writes writes;
    for (const External& patch : selected) {
        Writes ours;
        try {
            for (const XmlElement* line : patch.meta->Iter("Line")) {
                const std::string address = line->Get("Address", "");
                const auto value = ParseInt(address);
                if (!value) throw Error("invalid literal for int() with base 0: '" + address + "'");
                const std::uint64_t offset = std::uint64_t(*value) - kBase;
                Bytes data = Encode(*line);
                if (!segments.Contains(offset, data.size())) throw Error("address " + address + " is outside the eboot");
                ours.emplace_back(offset, std::move(data));
            }
        } catch (const Error& error) {
            Log("Patches: skipped %s: %s", patch.key.c_str(), error.what());
            continue;
        }
        if (BreaksOnline(ours)) {
            SkipForOnline(patch.key);
            continue;
        }
        if (resolution && Env("BB_RESOLUTION_PATCH", "") != "keep" &&
            std::any_of(ours.begin(), ours.end(), [&](const Write& w) {
                for (std::size_t i = 0; i < w.second.size(); ++i)
                    if (resolution->first.count(w.first + i)) return true;
                return false;
            })) {
            Log("Patches: skipped %s: it sets the game's resolution and UI layout, which the output resolution (%dx%d) "
                "already does; it applies again with a 1920x1080 output",
                patch.key.c_str(), resolution->second.w, resolution->second.h);
            continue;
        }
        writes.insert(writes.end(), ours.begin(), ours.end());
        Log("Patches: external %s (%zu writes)", patch.key.c_str(), ours.size());
    }
    return writes;
}

Writes ResolutionWrites(const PatchFile& xml, Size size, const std::string& app_version, const Segments& segments, Size ui) {
    const Writes writes = xml.Compile({kResolutionTemplate}, app_version, segments);
    const std::map<std::uint64_t, std::tuple<std::uint8_t, std::uint32_t, std::uint32_t>> replacements{
        {0x02196A6B - kBase, {0xB8, 0x500, std::uint32_t(size.w)}},
        {0x02196A7A - kBase, {0xB8, 0x2D0, std::uint32_t(size.h)}},
        {0x02358554 - kBase, {0xB8, 0x500, std::uint32_t(ui.w)}},
        {0x0235855D - kBase, {0xB9, 0x2D0, std::uint32_t(ui.h)}}};
    Writes out;
    std::set<std::uint64_t> seen;
    for (const auto& [offset, data] : writes) {
        const auto it = replacements.find(offset);
        if (it == replacements.end()) {
            out.emplace_back(offset, data);
            continue;
        }
        const auto [opcode, old, value] = it->second;
        const Bytes expected{opcode, std::uint8_t(old), std::uint8_t(old >> 8), std::uint8_t(old >> 16)};
        if (data != expected || seen.count(offset))
            throw Error("unexpected resolution patch at " + Hex(offset + kBase));
        out.emplace_back(offset, Bytes{opcode, std::uint8_t(value), std::uint8_t(value >> 8), std::uint8_t(value >> 16)});
        seen.insert(offset);
    }
    if (seen.size() != replacements.size()) throw Error("resolution patch is missing scene/UI viewport instructions");
    return out;
}

} // namespace

std::optional<Size> ParseSize(std::string_view text) {
    const auto parts = Split(Lower(std::string(text)), 'x');
    if (parts.size() != 2) return std::nullopt;
    int values[2];
    for (int i = 0; i < 2; ++i) {
        const std::string t = Trim(parts[std::size_t(i)]);
        if (t.empty() || t.size() > 9 || t.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
        values[i] = std::stoi(t);
    }
    return Size{values[0], values[1]};
}

std::optional<std::pair<Size, Size>> ScaledSizes(const std::map<std::string, std::string>& settings) {
    const Size out = OutputSize(settings);
    if (out == kOutput || Setting(settings, "upscaler") == "taa") return std::nullopt;
    double scale = 1.0;
    if (Setting(settings, "upscaler").value_or("fsr3") != "off") scale = PresetScale(settings);
    // A scene of at least 640x360, a third of 1080p: Ultra Performance at a 720p output made 428x240,
    // where a ROG Ally showed the world in half the picture (0.9.37 beta 7), and the upscaler has
    // little to work from below it.
    scale = std::max(1.0, std::min({scale, out.w / 640.0, out.h / 360.0}));
    Size render{std::max(4, Round(out.w / scale / 4) * 4), std::max(4, Round(out.h / scale / 4) * 4)};
    if (render == kOutput) render = {1916, 1076};
    return std::pair{render, out};
}

PatchResult CompilePatches(const PatchRequest& request) {
    const std::string app_version = "01.09";
    const auto settings = ReadSettings(request.settings);
    const PatchFile xml(request.xml);
    const auto& presets = FpsPresets();
    const std::map<std::string, int> scaled_fps{{"120", 120}, {"144", 144}};
    const auto preset_it = presets.find(request.fps);
    if (preset_it == presets.end() && !scaled_fps.count(request.fps)) throw Error("invalid FPS preset " + request.fps);
    const std::vector<std::string> preset =
        preset_it != presets.end() ? preset_it->second : std::vector<std::string>{kScaledBase, kSprint};
    std::vector<std::string> names = preset;
    const auto has = [&](const std::string& n) { return std::find(names.begin(), names.end(), n) != names.end(); };
    for (const std::string& n : Split(request.extra, ';'))
        if (!Trim(n).empty()) names.push_back(Trim(n));
    for (const std::string& n : EffectPatches(settings))
        if (!has(n)) names.push_back(n);
    std::vector<std::string> selected;
    for (const std::string& n : BuiltinSelection(xml, request.patches_config))
        if (!has(n)) selected.push_back(n);
    names.insert(names.end(), selected.begin(), selected.end());
    if (Env("BB_INTEL_TONEMAP_FIX", "") == "1" && !has(kIntelTonemap)) names.push_back(kIntelTonemap);
    if (Env("BB_SFX_WORKAROUND", "") == "1" && !has(kSfxWorkaround)) names.push_back(kSfxWorkaround);

    if (has("Restore Debug Camera") && has("Enemy Control"))
        throw Error("Restore Debug Camera conflicts with Enemy Control; enable only one");
    if (has("Restore Debug Menu (READ NOTES)")) {
        const fs::path font = request.game / "dvdroot_ps4" / "font";
        std::string missing;
        std::error_code error;
        for (const char* n : {"DbgFont14h.ccm", "DbgFont14h.tpf"})
            if (!fs::is_regular_file(font / n, error) || fs::file_size(font / n, error) == 0)
                missing += (missing.empty() ? "" : ", ") + std::string(n);
        // bbport: request.game is the merged mod view (a new mod-game-* folder each start): the
        // message names where the player puts them, not that folder. Mods that carry the font
        // unpacked (a DbgFont14h-tpf-dcx folder from WitchyBND) do not count: the game reads the
        // packed .tpf and .ccm.
        const bool unpacked = fs::is_directory(font / "DbgFont14h-tpf-dcx", error) ||
                              fs::is_directory(font / "DbgFont14h-tpf", error);
        if (!missing.empty())
            throw Error("Debug menu needs the font files " + missing +
                        " in dvdroot_ps4\\font, in the game folder or in an enabled mod (Mods page)." +
                        (unpacked ? std::string(" The installed mod has the font unpacked (a DbgFont14h-tpf-dcx "
                                                "folder): repack it with WitchyBND, or use the packed files")
                                  : std::string()) +
                        ". The fonts: https://www.nexusmods.com/bloodborne/mods/253; or turn the debug menu off "
                        "(debug_menu in lantern.ini)");
    }

    const Bytes elf = ReadFile(request.out / "eboot.elf");
    const Segments segments = EbootSegments(elf);
    {
        std::vector<std::string> kept;
        for (const std::string& n : names) {
            if (std::find(selected.begin(), selected.end(), n) != selected.end()) {
                try {
                    xml.Compile({n}, app_version, segments);
                } catch (const Error& error) {
                    std::string message = error.what();
                    if (StartsWith(message, n + ": ")) message.erase(0, n.size() + 2);
                    Log("Patches: skipped %s/%s: %s", Utf8(request.xml.filename()).c_str(), n.c_str(), message.c_str());
                    continue;
                }
            }
            kept.push_back(n);
        }
        names = kept;
    }
    names = CheckedPatches(xml, names, app_version, segments, elf, preset);
    {
        std::vector<std::string> kept;
        for (const std::string& n : names) {
            if (BreaksOnline(xml.Compile({n}, app_version, segments))) {
                SkipForOnline(n);
                continue;
            }
            kept.push_back(n);
        }
        names = kept;
    }
    Writes writes = xml.Compile(names, app_version, segments);
    if (scaled_fps.count(request.fps) && has(kScaledBase)) {
        writes = RateScaled(writes, scaled_fps.at(request.fps));
        Log("Patches: %s FPS: %s with its frame-rate values for %s", request.fps.c_str(), kScaledBase, request.fps.c_str());
    }
    if (!preset.empty() && has(preset[0])) {
        std::optional<int> fps;
        if (request.fps == "60") fps = 60;
        else if (request.fps == "90") fps = 90;
        else if (scaled_fps.count(request.fps)) fps = scaled_fps.at(request.fps);
        const std::string tuning = Lower(Env("BB_CAMERA_TUNING", Setting(settings, "camera_tuning").value_or("original")));
        writes = FrameRateFixes(writes, fps, elf, tuning != "fps++");
        if (fps) Log("Patches: follow camera %s", tuning != "fps++" ? "as the game has it, its eases converted from 30 FPS"
                                                                   : "with the FPS++ retune (camera_tuning=fps++)");
    }
    if (Env("BB_DLC", "") == "1") {
        const Bytes was = FromHex("e8bb210000");
        if (OriginalBytes(elf, 0x1fb6d20, was.size()) == was)
            writes.emplace_back(0x1fb6d20, FromHex("b801000000"));
        else
            Log("Patches: The Old Hunters edition left alone: this executable differs there");
    }
    if (Env("BB_RENDER_POOLS", "") == "0") {
        Log("Patches: render pools at the game's 256 KiB (BB_RENDER_POOLS=0)");
    } else {
        const Bytes code = FromHex("ba00000400");
        const std::uint64_t sites[] = {0x2195c09, 0x2195c46};
        if (std::all_of(std::begin(sites), std::end(sites),
                        [&](std::uint64_t s) { return OriginalBytes(elf, s, code.size()) == code; })) {
            for (std::uint64_t s : sites) writes.emplace_back(s + 1, Le<std::uint32_t>(0x100000));
            Log("Patches: render pools 1024 KiB each (the game's 256)");
        } else {
            Log("Patches: render pools left at the game's size: this executable differs there");
        }
    }
    const std::optional<Size> size = request.render_res.empty() ? std::nullopt : RenderSize(settings, request.render_res);
    Size out = OutputSize(settings);
    if (!request.output_res.empty()) {
        const auto parsed = ParseSize(request.output_res);
        if (!parsed) throw Error("invalid output resolution " + request.output_res);
        out = *parsed;
    }
    const Size ui = size ? UiSize(settings, out) : kOutput;
    if (size) {
        const Writes resolution = ResolutionWrites(xml, *size, app_version, segments, ui);
        writes.insert(writes.end(), resolution.begin(), resolution.end());
        Log("Patches: scene %dx%d; UI %dx%d", size->w, size->h, ui.w, ui.h);
    }
    // The game sizes its two graphics heaps for the PS4: 2287 and 886 MiB. A scene above 1080p,
    // highest model detail or screen space reflections outgrow them, and a model that no longer
    // fits is not drawn (ladders, ground, the Astral Clocktower's clock missing). The engine gives
    // them 3952 and 2560 MiB and direct memory to match (main.cpp: 9152 MiB) when the scene needs
    // them: above 1080p, highest model detail or screen space reflections. Kept at the PS4 size
    // otherwise, and with BB_GAME_HEAPS=0, BB_DMEM_MB under 9152 or a commit limit (RAM plus page
    // file) under 16 GiB unless the scene is above 1080p.
    {
        const bool large_scene = size && std::int64_t(size->w) * size->h > std::int64_t(kOutput.w) * kOutput.h;
        // Lantern Engine: only what outgrows the PS4 heaps enlarges them. Every heap page the
        // game touches stays in RAM for the session (its allocators spread over the whole heap),
        // so larger heaps meant up to 3.3 GB more RAM: 7 GB of game memory and Windows swapping
        // on 16 GB PCs (the multi-second hangs of 2026-10-21). BB_GAME_HEAPS=1 enlarges anyway.
        const bool needs = large_scene || std::find(names.begin(), names.end(), "Model LOD -2 (Highest)") != names.end() ||
                           std::find(names.begin(), names.end(), "Enable Screen Space Reflections (READ NOTE)") != names.end();
        const std::uint64_t commit = CommitLimitMb();
        const auto dmem = Env("BB_DMEM_MB");
        std::string keep;
        if (Env("BB_GAME_HEAPS", "") == "0")
            keep = "BB_GAME_HEAPS=0";
        else if (!needs && Env("BB_GAME_HEAPS", "") != "1")
            keep = "a 1080p or smaller scene without highest model detail or screen space reflections fits them; "
                   "BB_GAME_HEAPS=1 enlarges them";
        else if (dmem && std::strtoull(dmem->c_str(), nullptr, 10) < 9152)
            keep = "BB_DMEM_MB=" + *dmem;
        else if (commit && commit < 16384)
            keep = "commit limit " + std::to_string(commit) + " MiB, RAM plus page file, under 16 GiB";
        // Enlarged heaps need the 9152 MiB pool; a PC that refuses it would crash at the start
        // (GitHub #66: error 1450 on a laptop), whatever the scene size.
        bool refused = false;
        if (keep.empty()) {
            if (const unsigned long error = PoolRefused(9152)) {
                keep = "Windows refused the 9152 MiB memory pool they need, error " + std::to_string(error) +
                       "; more page file or free RAM lets them grow";
                refused = true;
            }
        }
        if (!keep.empty() && (!needs || refused || Env("BB_GAME_HEAPS", "") == "0")) {
            Log("Patches: the game's graphics heaps at their PS4 size (%s)", keep.c_str());
        } else if (OriginalBytes(elf, kHeapSizes, 8) == Le<std::uint64_t>(0x8ef00000) &&
                   OriginalBytes(elf, kHeapSizes + 8, 8) == Le<std::uint64_t>(0x37600000)) {
            writes.emplace_back(kHeapSizes, Le<std::uint64_t>(0xf7000000));
            writes.emplace_back(kHeapSizes + 8, Le<std::uint64_t>(0xa0000000));
            Log("Patches: the game's graphics heaps 3952 and 2560 MiB (the PS4's 2287 and 886)");
        } else {
            Log("Patches: the game's graphics heaps left at their size: this executable differs there");
        }
    }
    if (!request.patches_dir.empty()) {
        std::optional<std::pair<std::set<std::uint64_t>, Size>> resolution;
        if (!(out == kOutput)) {
            std::set<std::uint64_t> footprint;
            for (const auto& [offset, data] : xml.Compile({kResolutionTemplate}, app_version, segments))
                for (std::size_t i = 0; i < data.size(); ++i) footprint.insert(offset + i);
            resolution = std::pair{std::move(footprint), out};
        }
        std::vector<std::unique_ptr<PatchFile>> files;
        std::vector<External> found;
        ExternalPatches(request.patches_dir, app_version, request.xml, files, found);
        nlohmann::json config = nlohmann::json::object();
        try {
            config = ReadConfig(request.patches_config);
        } catch (const std::exception& error) {
            throw Error(std::string("patches.json: ") + error.what());
        }
        std::set<std::string> enabled, disabled;
        for (const char* k : {"enabled", "disabled"})
            if (config.contains(k))
                for (const auto& v : config.at(k))
                    if (v.is_string()) (std::string(k) == "enabled" ? enabled : disabled).insert(v.get<std::string>());
        std::vector<External> chosen;
        for (const External& e : found)
            if (enabled.count(e.key) || (!disabled.count(e.key) && Lower(e.meta->Get("isEnabled", "false")) == "true"))
                chosen.push_back(e);
        const Writes external = CompileExternal(chosen, segments, resolution);
        writes.insert(writes.end(), external.begin(), external.end());
    }
    if (Env("BB_OTHER_EDITION", "") == "1") {
        writes.clear();
        names.clear();
        Log("Patches: none: this edition's executable is not the one they were made for (CUSA03173 1.09)");
    }
    PatchResult result;
    result.ui = Env("BB_OTHER_EDITION", "") == "1" ? kOutput : ui;
    result.heap_patch = std::any_of(writes.begin(), writes.end(), [&](const Write& w) {
        return w.first < kHeapSizes + 16 && w.first + w.second.size() > kHeapSizes;
    });
    // The GX range allocator: GPU memory for buffers the CPU updates (per-draw shader parameters,
    // CPU-deformed vertices). Ebrietas' trail effect drove its 160 MiB to 0.5 MB free right before
    // her device losses, and two per-frame passes (0xeb75e0, 0x21d921d) write through stale
    // pointers when it is full. BB_GX_REGION_MB=<MiB> (160-416, default 288; 0 keeps the game's).
    // With the heaps at the PS4 size GFX_GraphicsPrivate grows by as much: the 5056 MiB pool holds
    // it (the game's direct memory ends near 4.6 GB), so heap_patch above stays as it was.
    if (Env("BB_OTHER_EDITION", "") != "1") {
        const std::string value = Env("BB_GX_REGION_MB", "");
        unsigned mib = kGxRegionDefault;
        if (!value.empty() && std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; }))
            mib = unsigned(std::min<unsigned long long>(std::strtoull(value.c_str(), nullptr, 10), 100000));
        const Bytes code = Concat({FromHex("c78388000000"), Le<std::uint32_t>(0x8000000)});
        if (mib <= kGxRegionGame) {
            Log("Patches: the GX range allocator at the game's %u MiB (BB_GX_REGION_MB)", kGxRegionGame);
        } else if (OriginalBytes(elf, kGxRegion, code.size()) != code ||
                   (!result.heap_patch && OriginalBytes(elf, kHeapSizes, 8) != Le<std::uint64_t>(kGxHeapPs4))) {
            Log("Patches: the GX range allocator left at the game's size: this executable differs there");
        } else {
            mib = std::min(mib, kGxRegionMax);
            writes.emplace_back(kGxRegion + 6, Le<std::uint32_t>((mib - kGxRegionGame + 128) << 20));
            if (result.heap_patch) {
                Log("Patches: the GX range allocator %u MiB (the game's %u)", mib, kGxRegionGame);
            } else {
                writes.emplace_back(kHeapSizes, Le<std::uint64_t>(kGxHeapPs4 + (std::uint64_t(mib - kGxRegionGame) << 20)));
                Log("Patches: the GX range allocator %u MiB (the game's %u), the graphics heap %llu MiB", mib,
                    kGxRegionGame, (unsigned long long)((kGxHeapPs4 >> 20) + mib - kGxRegionGame));
            }
        }
    }
    Bytes blob;
    Append(blob, "BBPATCH2", 8);
    Append<std::uint64_t>(blob, kBase);
    Append<std::uint64_t>(blob, writes.size());
    for (const auto& [offset, data] : writes) {
        Append<std::uint64_t>(blob, offset);
        Append<std::uint64_t>(blob, data.size());
        Append(blob, data.data(), data.size());
    }
    WriteFile(request.out / "patches.bin", blob);
    Log("Patches: FPS preset %s; %zu writes from %s", request.fps.c_str(), writes.size(),
        names.empty() ? "none" : Repr(names).c_str());
    return result;
}

} // namespace start
