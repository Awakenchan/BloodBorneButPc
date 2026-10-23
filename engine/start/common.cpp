// SPDX-License-Identifier: GPL-3.0-or-later
#include "common.h"

#include <windows.h>
#include <bcrypt.h>
#include <zlib.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <fstream>

namespace start {

fs::path Path(std::string_view utf8) {
    return fs::path(Wide(utf8));
}

std::string Utf8(const fs::path& path) {
    return Utf8(std::wstring_view(path.native()));
}

std::string Utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(std::size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Wide(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    std::wstring out(std::size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), n);
    return out;
}

Bytes ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Error("cannot read " + Utf8(path));
    in.seekg(0, std::ios::end);
    const auto size = std::streamoff(in.tellg());
    in.seekg(0);
    Bytes data(std::size_t(std::max<std::streamoff>(size, 0)));
    if (!data.empty() && !in.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size())))
        throw Error("cannot read " + Utf8(path));
    return data;
}

std::string ReadText(const fs::path& path) {
    const Bytes data = ReadFile(path);
    std::string text(data.begin(), data.end());
    if (StartsWith(text, "\xEF\xBB\xBF")) text.erase(0, 3);
    return text;
}

void WriteFile(const fs::path& path, const void* data, std::size_t size) {
    if (path.has_parent_path()) fs::create_directories(path.parent_path());
    fs::path temporary = path;
    temporary += L".tmp" + std::to_wstring(GetCurrentProcessId());
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out || (size && !out.write(static_cast<const char*>(data), std::streamsize(size))))
            throw Error("cannot write " + Utf8(path));
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temporary.c_str());
        throw Error("cannot write " + Utf8(path));
    }
}

namespace {
Bytes Hash(LPCWSTR id, const void* data, std::size_t size) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD length = 0, got = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, id, nullptr, 0) < 0) throw Error("no hash provider");
    BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&length), sizeof length, &got, 0);
    Bytes digest(length);
    BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    const auto* p = static_cast<const std::uint8_t*>(data);
    while (size) {
        const ULONG chunk = ULONG(std::min<std::size_t>(size, 1u << 30));
        BCryptHashData(hash, const_cast<PUCHAR>(p), chunk, 0);
        p += chunk;
        size -= chunk;
    }
    BCryptFinishHash(hash, digest.data(), length, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return digest;
}
}

std::string Sha256Hex(const void* data, std::size_t size) {
    const Bytes digest = Hash(BCRYPT_SHA256_ALGORITHM, data, size);
    return ToHex(digest.data(), digest.size());
}

Bytes Sha1(const void* data, std::size_t size) {
    return Hash(BCRYPT_SHA1_ALGORITHM, data, size);
}

Sha256Stream::Sha256Stream() {
    BCryptOpenAlgorithmProvider(reinterpret_cast<BCRYPT_ALG_HANDLE*>(&algorithm_), BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    BCryptCreateHash(algorithm_, reinterpret_cast<BCRYPT_HASH_HANDLE*>(&hash_), nullptr, 0, nullptr, 0, 0);
}

Sha256Stream::~Sha256Stream() {
    if (hash_) BCryptDestroyHash(hash_);
    if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
}

void Sha256Stream::Update(const void* data, std::size_t size) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    while (size) {
        const ULONG chunk = ULONG(std::min<std::size_t>(size, 1u << 30));
        BCryptHashData(hash_, const_cast<PUCHAR>(p), chunk, 0);
        p += chunk;
        size -= chunk;
    }
}

std::string Sha256Stream::HexDigest() {
    std::uint8_t digest[32];
    BCryptFinishHash(hash_, digest, sizeof digest, 0);
    return ToHex(digest, sizeof digest);
}

std::uint32_t Crc32(const void* data, std::size_t size) {
    return std::uint32_t(crc32(0, static_cast<const Bytef*>(data), uInt(size)));
}

std::optional<std::string> Env(const char* name) {
    const std::wstring key = Wide(name);
    const DWORD n = GetEnvironmentVariableW(key.c_str(), nullptr, 0);
    if (!n) return std::nullopt;
    std::wstring value(n, L'\0');
    const DWORD got = GetEnvironmentVariableW(key.c_str(), value.data(), n);
    value.resize(got);
    return Utf8(value);
}

std::string Env(const char* name, std::string_view fallback) {
    const auto value = Env(name);
    return value ? *value : std::string(fallback);
}

std::uint64_t CommitLimitMb() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? status.ullTotalPageFile >> 20 : 0;
}

namespace {
struct PoolAnswer {
    unsigned long error = 0;
    HANDLE section = nullptr;
    std::uint64_t bytes = 0;
};
std::map<std::uint64_t, PoolAnswer>& PoolAnswers() {
    static std::map<std::uint64_t, PoolAnswer> answers;
    return answers;
}
}

unsigned long PoolRefused(std::uint64_t mib) {
    auto& answers = PoolAnswers();
    if (const auto it = answers.find(mib); it != answers.end()) return it->second.error;
    const std::uint64_t size = (mib + 1024) << 20;
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    const bool lazy = Env("BB_POOL_LAZY", "") == "1";
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, &inherit,
                                        PAGE_EXECUTE_READWRITE | (lazy ? SEC_RESERVE : SEC_COMMIT),
                                        DWORD(size >> 32), DWORD(size), nullptr);
    PoolAnswer& answer = answers[mib];
    answer.error = section ? 0 : GetLastError();
    answer.section = section;
    answer.bytes = size;
    return answer.error;
}

void PoolHandOff(std::uint64_t mib) {
    UnsetEnv("BB_POOL_SECTION");
    for (auto& [size, answer] : PoolAnswers()) {
        if (!answer.section) continue;
        if (size == mib) {
            char text[64];
            std::snprintf(text, sizeof text, "%llu:%llu", (unsigned long long)(std::uintptr_t)answer.section,
                          (unsigned long long)answer.bytes);
            SetEnv("BB_POOL_SECTION", text);
        } else {
            CloseHandle(answer.section);
            answer.section = nullptr;
        }
    }
}

void SetEnv(const char* name, std::string_view value) {
    SetEnvironmentVariableW(Wide(name).c_str(), Wide(value).c_str());
}

void UnsetEnv(const char* name) {
    SetEnvironmentVariableW(Wide(name).c_str(), nullptr);
}

std::map<std::string, std::string> ReadSettings(const fs::path& path) {
    std::map<std::string, std::string> settings;
    std::error_code error;
    if (!fs::is_regular_file(path, error)) return settings;
    for (const std::string& line : Split(ReadText(path), '\n')) {
        const std::string text = Trim(line);
        if (text.empty() || text[0] == '#' || text[0] == ';') continue;
        const auto equals = text.find('=');
        if (equals == std::string::npos) continue;
        settings[Trim(text.substr(0, equals))] = Trim(text.substr(equals + 1));
    }
    return settings;
}

std::optional<std::string> Setting(const std::map<std::string, std::string>& settings, const std::string& key) {
    const auto it = settings.find(key);
    return it == settings.end() ? std::nullopt : std::optional<std::string>(it->second);
}

namespace {
HANDLE g_log = nullptr;
}

void Log(const char* format, ...) {
    char stack[2048];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(stack, sizeof stack, format, args);
    va_end(args);
    std::string text;
    if (n >= int(sizeof stack)) {
        text.resize(std::size_t(n) + 1);
        va_start(args, format);
        std::vsnprintf(text.data(), text.size(), format, args);
        va_end(args);
        text.resize(std::size_t(n));
    } else if (n > 0) {
        text.assign(stack, std::size_t(n));
    }
    text += '\n';
    if (g_log) {
        DWORD written = 0;
        ::WriteFile(g_log, text.data(), DWORD(text.size()), &written, nullptr);
    } else {
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fflush(stdout);
    }
}

void InitLog() {
    const auto path = Env("BB_LOG_FILE");
    if (!path || path->empty()) return;
    const fs::path file = Path(*path);
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    g_log = CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        &inherit, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_log == INVALID_HANDLE_VALUE) {
        g_log = nullptr;
        return;
    }
    SYSTEMTIME now;
    GetLocalTime(&now);
    Log("%s", std::string(80, '=').c_str());
    Log("Bloodborne PC session log started: %04u-%02u-%02uT%02u:%02u:%02u", now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond);
    Log("Log file: %s", Utf8(fs::absolute(file, error)).c_str());
    Log("%s", std::string(80, '=').c_str());
}

void* LogHandle() {
    return g_log;
}

std::string Hex(std::uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof text, "%#llx", static_cast<unsigned long long>(value));
    return value ? text : "0x0";
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

std::string Trim(std::string_view text) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; };
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    while (!text.empty() && space(text.back())) text.remove_suffix(1);
    return std::string(text);
}

std::vector<std::string> Split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const auto at = text.find(separator, start);
        parts.emplace_back(text.substr(start, at == std::string_view::npos ? std::string_view::npos : at - start));
        if (at == std::string_view::npos) break;
        start = at + 1;
    }
    return parts;
}

bool StartsWith(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

Bytes FromHex(std::string_view hex) {
    Bytes out;
    int high = -1;
    for (char c : hex) {
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else if (c == ' ') continue;
        else throw Error("bad hex digit");
        if (high < 0) {
            high = v;
        } else {
            out.push_back(std::uint8_t(high << 4 | v));
            high = -1;
        }
    }
    if (high >= 0) throw Error("odd hex length");
    return out;
}

std::string ToHex(const std::uint8_t* data, std::size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string out(size * 2, '0');
    for (std::size_t i = 0; i < size; ++i) {
        out[2 * i] = digits[data[i] >> 4];
        out[2 * i + 1] = digits[data[i] & 15];
    }
    return out;
}

}
