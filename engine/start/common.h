// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

namespace start {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

fs::path Path(std::string_view utf8);
std::string Utf8(const fs::path& path);
std::string Utf8(std::wstring_view text);
inline std::string Utf8(const std::wstring& text) { return Utf8(std::wstring_view(text)); }
inline std::string Utf8(const wchar_t* text) { return Utf8(std::wstring_view(text)); }
std::wstring Wide(std::string_view utf8);

Bytes ReadFile(const fs::path& path);
std::string ReadText(const fs::path& path);
void WriteFile(const fs::path& path, const void* data, std::size_t size);
inline void WriteFile(const fs::path& path, const Bytes& data) { WriteFile(path, data.data(), data.size()); }
inline void WriteText(const fs::path& path, std::string_view text) { WriteFile(path, text.data(), text.size()); }

std::string Sha256Hex(const void* data, std::size_t size);
inline std::string Sha256Hex(const Bytes& data) { return Sha256Hex(data.data(), data.size()); }
Bytes Sha1(const void* data, std::size_t size);
class Sha256Stream {
public:
    Sha256Stream();
    ~Sha256Stream();
    void Update(const void* data, std::size_t size);
    void Update(std::string_view text) { Update(text.data(), text.size()); }
    std::string HexDigest();
private:
    void* hash_ = nullptr;
    void* algorithm_ = nullptr;
};
std::uint32_t Crc32(const void* data, std::size_t size);

std::optional<std::string> Env(const char* name);
std::string Env(const char* name, std::string_view fallback);
void SetEnv(const char* name, std::string_view value);
void UnsetEnv(const char* name);

std::uint64_t CommitLimitMb();
unsigned long PoolRefused(std::uint64_t mib);
void PoolHandOff(std::uint64_t mib);

std::map<std::string, std::string> ReadSettings(const fs::path& path);
std::optional<std::string> Setting(const std::map<std::string, std::string>& settings, const std::string& key);

void Log(const char* format, ...)
#if defined(__clang__) || defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
void InitLog();
void* LogHandle();

std::string Hex(std::uint64_t value);
std::string Lower(std::string text);
std::string Trim(std::string_view text);
std::vector<std::string> Split(std::string_view text, char separator);
bool StartsWith(std::string_view text, std::string_view prefix);
Bytes FromHex(std::string_view hex);
std::string ToHex(const std::uint8_t* data, std::size_t size);

template <typename T> T Load(const std::uint8_t* data) {
    T value;
    std::memcpy(&value, data, sizeof value);
    return value;
}
template <typename T> void Store(std::uint8_t* data, T value) { std::memcpy(data, &value, sizeof value); }
template <typename T> void Append(Bytes& out, T value) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), p, p + sizeof value);
}
inline void Append(Bytes& out, const void* data, std::size_t size) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    out.insert(out.end(), p, p + size);
}

}
