// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/menu_memory.h"
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
constexpr std::size_t kHeap = 0x1f5a465, kPages = 0x1f5a479, kBudget = 0x4736d40 + 2 * 0x80 + 9 * 8;
template <typename T> T At(const std::vector<std::uint8_t>& b, std::size_t at) {
    T v;
    std::memcpy(&v, b.data() + at, sizeof v);
    return v;
}
template <typename T> void Put(std::vector<std::uint8_t>& b, std::size_t at, T v) { std::memcpy(b.data() + at, &v, sizeof v); }
}

int main() {
    std::vector<std::uint8_t> image(kBudget + 8, 0);
    image[kHeap] = image[kPages] = 0xba;
    Put<std::uint32_t>(image, kHeap + 1, 18u << 20);
    Put<std::uint32_t>(image, kPages + 1, 34u << 20);
    Put<std::uint64_t>(image, kBudget, 82ull << 20);

    Check(!BbEngine::MenuMemory::Install(image.data(), image.size() - 1), "a short image is refused");
    image[kPages] = 0x90;
    Check(!BbEngine::MenuMemory::Install(image.data(), image.size()), "a changed site is refused");
    Check(At<std::uint32_t>(image, kHeap + 1) == 18u << 20 && At<std::uint64_t>(image, kBudget) == 82ull << 20,
          "a refusal leaves the other sites alone");
    image[kPages] = 0xba;
    Check(BbEngine::MenuMemory::Compatible(image.data(), image.size()), "1.09's sizes are compatible");
    Check(BbEngine::MenuMemory::Install(image.data(), image.size()), "1.09's sizes are enlarged");
    Check(At<std::uint32_t>(image, kHeap + 1) == 50u << 20 && At<std::uint32_t>(image, kPages + 1) == 98u << 20 &&
              At<std::uint64_t>(image, kBudget) == 178ull << 20,
          "heaps 50 and 98 MiB, MENU 178 MiB");
    Check(!BbEngine::MenuMemory::Install(image.data(), image.size()), "never enlarged twice");
    if (failures) return 1;
    std::puts("PASS: menu heaps: bounds, another patch refused, all or none, no second enlargement");
    return 0;
}
