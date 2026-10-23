// SPDX-License-Identifier: GPL-2.0-or-later
#include "sha256.h"

#include <array>
#include <cstring>

namespace {
constexpr std::array<std::uint32_t, 64> kRound = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr std::uint32_t Rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct State {
    std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    void Block(const std::uint8_t* p) {
        std::uint32_t w[64];
        for (int t = 0; t < 16; ++t)
            w[t] = std::uint32_t(p[4 * t]) << 24 | std::uint32_t(p[4 * t + 1]) << 16 |
                   std::uint32_t(p[4 * t + 2]) << 8 | p[4 * t + 3];
        for (int t = 16; t < 64; ++t) {
            const std::uint32_t s0 = Rotr(w[t - 15], 7) ^ Rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
            const std::uint32_t s1 = Rotr(w[t - 2], 17) ^ Rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
            w[t] = w[t - 16] + s0 + w[t - 7] + s1;
        }
        auto v = h;
        for (int t = 0; t < 64; ++t) {
            const std::uint32_t e = v[4], a = v[0];
            const std::uint32_t t1 = v[7] + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) +
                                     ((e & v[5]) ^ (~e & v[6])) + kRound[t] + w[t];
            const std::uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) +
                                     ((a & v[1]) ^ (a & v[2]) ^ (v[1] & v[2]));
            for (int i = 7; i > 0; --i) v[i] = v[i - 1];
            v[4] += t1;
            v[0] = t1 + t2;
        }
        for (int i = 0; i < 8; ++i) h[i] += v[i];
    }
};
}

std::string sha256_hex(const std::uint8_t* data, std::size_t len) {
    State s;
    std::size_t done = 0;
    for (; len - done >= 64; done += 64) s.Block(data + done);
    std::uint8_t last[128]{};
    const std::size_t rest = len - done;
    if (rest) std::memcpy(last, data + done, rest);
    last[rest] = 0x80;
    const std::size_t blocks = rest < 56 ? 1 : 2;
    const std::uint64_t bits = std::uint64_t(len) * 8;
    for (int i = 0; i < 8; ++i) last[64 * blocks - 1 - i] = std::uint8_t(bits >> (8 * i));
    for (std::size_t b = 0; b < blocks; ++b) s.Block(last + 64 * b);

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const std::uint32_t word : s.h)
        for (int shift = 28; shift >= 0; shift -= 4) out += kHex[(word >> shift) & 0xf];
    return out;
}
