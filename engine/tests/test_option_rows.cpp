// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/option_rows.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define GUEST __attribute__((sysv_abi))
namespace R = BbEngine::OptionRows;
using u64 = std::uint64_t;

namespace {
template <typename T> T Get(const void* at) {
    T v{};
    std::memcpy(&v, at, sizeof v);
    return v;
}
template <typename T> void Put(void* at, T v) { std::memcpy(at, &v, sizeof v); }
std::uint8_t* P(u64 a) { return reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(a)); }

int g_heap = 0;
void GUEST Free(u64, u64 text) {
    std::free(P(text));
    --g_heap;
}
u64 g_allocator_vtable[16];
u64 g_allocator[1];
char16_t* HeapCopy(const std::u16string& s) {
    auto* p = static_cast<char16_t*>(std::malloc((s.size() + 1) * 2));
    std::memcpy(p, s.c_str(), (s.size() + 1) * 2);
    ++g_heap;
    return p;
}
void MakeString(std::uint8_t* s, const std::u16string& text) {
    Put<u64>(s + 0x28, u64(g_allocator));
    Put<u64>(s + 0x18, text.size());
    if (text.size() >= 8) {
        Put<u64>(s + 0x20, text.size());
        Put(s + 0x08, HeapCopy(text));
    } else {
        Put<u64>(s + 0x20, 7);
        std::memcpy(s + 0x08, text.c_str(), (text.size() + 1) * 2);
    }
}
std::u16string ReadString(const std::uint8_t* s) {
    const auto n = Get<u64>(s + 0x18);
    const char16_t* t = Get<u64>(s + 0x20) >= 8 ? Get<const char16_t*>(s + 0x08) : reinterpret_cast<const char16_t*>(s + 0x08);
    return std::u16string(t, n);
}

std::map<std::pair<u64, u64>, std::u16string> g_texts;
void GUEST Message(std::uint8_t* out, u64 group, u64 id) {
    std::memset(out, 0, 0x40);
    out[0x38] = 1;
    if (id == 404) {
        MakeString(out + 0x08, u"<?menu 404?>");
        return;
    }
    std::string name = std::to_string(group) + ":" + std::to_string(id);
    auto& text = g_texts[{group, id}];
    text.assign(name.begin(), name.end());
    Put(out, text.c_str());
    MakeString(out + 0x08, u"");
}
void GUEST StringFrom(std::uint8_t* s, const char16_t* text) { MakeString(s, text); }

void CopyEntry(std::uint8_t* to, const std::uint8_t* from) {
    std::memcpy(to, from, 0x48);
    MakeString(to + 0x10, ReadString(from + 0x10));
}
void GUEST ChoiceAppend(std::uint8_t* list, const std::uint8_t* entry) {
    const auto n = Get<u64>(list + 0x98);
    assert(n < 2);
    CopyEntry(list + n * 0x48, entry);
    Put<u64>(list + 0x98, n + 1);
}
void GUEST PickAppend(std::uint8_t* list, const std::uint8_t* entry) {
    const auto n = Get<u64>(list + 0x908);
    assert(n < 32);
    CopyEntry(list + n * 0x48, entry);
    Put<u64>(list + 0x908, n + 1);
}
void GUEST OnOff(std::uint8_t* list) {
    Put<u64>(list + 0x98, 0);
    for (const auto& [value, id] : {std::pair{1, 0x9d6c}, std::pair{0, 0x9d6d}}) {
        alignas(16) std::uint8_t m[0x40], e[0x48]{};
        Message(m, 200, id);
        e[0] = std::uint8_t(value);
        Put(e + 0x08, Get<u64>(m));
        MakeString(e + 0x10, Get<const char16_t*>(m));
        e[0x40] = 1;
        ChoiceAppend(list, e);
        Free(0, Get<u64>(e + 0x18));
    }
}

int g_widgets = 0;
void GUEST DeleteWidget(std::uint8_t* w) {
    delete[] w;
    --g_widgets;
}
u64 g_widget_vtable[1] = {u64(&DeleteWidget)};
struct Row {
    std::u16string label, help;
    std::vector<std::u16string> texts;
    std::int32_t def;
};
std::vector<Row> g_rows;
alignas(16) std::uint8_t g_records[16 * 0x90];
void Push(std::uint8_t* dialog, const std::uint8_t* captions, const void* value, std::size_t value_at, Row row) {
    auto end = Get<u64>(dialog + 0xe60);
    std::uint8_t* r = P(end);
    std::memcpy(r, captions, 0x80);
    MakeString(r + 0x08, u"caption copy " + ReadString(captions + 0x08));
    MakeString(r + 0x48, u"help copy " + ReadString(captions + 0x48));
    auto* w = new std::uint8_t[0xcf0]{};
    ++g_widgets;
    Put(w, u64(g_widget_vtable));
    Put<std::int32_t>(w + 8, 1);
    Put(w + value_at, value);
    Put(r + 0x80, u64(w));
    Put(dialog + 0xe60, end + 0x90);
    row.label = Get<const char16_t*>(captions);
    row.help = Get<const char16_t*>(captions + 0x40);
    g_rows.push_back(row);
}
std::vector<std::u16string> Texts(const std::uint8_t* list, u64 count) {
    std::vector<std::u16string> out;
    for (u64 i = 0; i < count; ++i) out.push_back(Get<const char16_t*>(list + i * 0x48 + 8));
    return out;
}
void GUEST Slider(std::uint8_t* d, std::uint8_t* c, std::uint8_t* v, std::uint8_t* def) { Push(d, c, v, 0xcd0, {{}, {}, {}, *def}); }
void GUEST Choice(std::uint8_t* d, std::uint8_t* c, std::uint8_t* v, std::uint8_t* list, std::uint8_t* def) {
    Push(d, c, v, 0x360, {{}, {}, Texts(list, Get<u64>(list + 0x98)), *def});
}
void GUEST Pick(std::uint8_t* d, std::uint8_t* c, std::int32_t* v, std::uint8_t* list, std::int32_t* def) {
    Push(d, c, v, 0x3c8, {{}, {}, Texts(list, Get<u64>(list + 0x908)), *def});
}
int g_layouts = 0, g_redraws = 0;
void GUEST Layout(std::uint8_t*, u64 zero) {
    assert(zero == 0);
    ++g_layouts;
}
void GUEST Redraw(std::uint8_t*) { ++g_redraws; }

u64 g_image = 0;
std::vector<std::u16string> g_commands;
void* g_opener = nullptr;
void CheckFunction(const std::uint8_t* f, std::size_t vtable) {
    assert(Get<u64>(f) == g_image + vtable && Get<const void*>(f + 0x20) == f);
}
void GUEST AddCommand(void*, std::uint8_t* c, std::uint8_t* f, u64* flag) {
    CheckFunction(f, 0x533d120);
    assert(flag && *flag == 0);
    g_commands.push_back(Get<const char16_t*>(c));
    g_opener = Get<void*>(f + 8);
}
void GUEST AddExit(void*, std::uint8_t* c, std::uint8_t* f) {
    CheckFunction(f, 0x533d120);
    g_commands.push_back(u"exit " + std::u16string(Get<const char16_t*>(c)));
}
struct Counted {
    u64 vtable = u64(g_widget_vtable);
    std::int32_t count = 1;
};
Counted g_box, g_command;
int g_yes = 0;
void GUEST QuestionBox(u64* box, void*, std::uint8_t* m) {
    assert(std::u16string(Get<const char16_t*>(m)) == u"204:920001");
    *box = u64(&g_box);
}
void GUEST Command(u64* command, std::uint8_t* f, u64 two) {
    CheckFunction(f, 0x5343880);
    assert(two == 2);
    reinterpret_cast<void(GUEST*)(std::int64_t, std::int64_t)>(Get<void*>(f + 8))(0, 0);
    *command = u64(&g_command);
}
void GUEST BoxWithCommand(void*, u64* box, u64* command) {
    ++P(*box)[8], ++P(*command)[8];
}
void GUEST Yes(std::int64_t, std::int64_t) { ++g_yes; }

std::uint8_t* Image() {
    constexpr std::size_t size = 0x2970000;
#ifdef _WIN32
    auto* image = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
#else
    auto* image = static_cast<std::uint8_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
#endif
    assert(image);
    const std::pair<std::size_t, const void*> stubs[] = {
        {0x1ae8cc0, (void*)&Message},      {0x2967040, (void*)&StringFrom}, {0x1b2ac00, (void*)&Slider},
        {0x1b2a100, (void*)&Choice},       {0x1b29370, (void*)&Pick},       {0x1b2b3b0, (void*)&OnOff},
        {0x1b2c200, (void*)&ChoiceAppend}, {0x1b1c0c0, (void*)&PickAppend}, {0x1ad06e0, (void*)&Redraw},
        {0x1b28de0, (void*)&Layout},       {0x1b4e2a0, (void*)&AddCommand}, {0x1b4c9a0, (void*)&AddExit},
        {0x1c1ed10, (void*)&QuestionBox},  {0x195db60, (void*)&Command},    {0x1c1a070, (void*)&BoxWithCommand},
    };
    for (const auto& [at, fn] : stubs) {
        image[at] = 0x48, image[at + 1] = 0xb8;
        Put(image + at + 2, fn);
        image[at + 10] = 0xff, image[at + 11] = 0xe0;
    }
    return image;
}
void* GUEST Opener(void* out, void*) { return out; }
}

int main() {
    g_allocator_vtable[0x70 / 8] = u64(&Free);
    g_allocator[0] = u64(g_allocator_vtable);
    std::uint8_t* image = Image();
    g_image = u64(image);
    R::Bind(g_image);

    assert(std::u16string(R::Message(110006)) == u"200:110006" && std::u16string(R::Message(7, 201)) == u"201:7");
    assert(!R::Message(404) && g_heap == 0);
    {
        R::Captions c(118000, 404);
        assert(g_heap == 1);
    }
    assert(g_heap == 0);

    alignas(16) static std::uint8_t dialog[0xf20]{};
    Put(dialog + 0xe58, u64(g_records));
    Put(dialog + 0xe60, u64(g_records));
    Put(dialog + 0xa70, g_image + 0x5399fb0);
    std::uint8_t toggle = 1, slider = 4, text = 9;
    std::int32_t pick = 2;
    const R::Choice choices[] = {{0, 117010}, {1, 117011}, {2, 117012}};
    R::AddToggle(dialog, &toggle, 124001, 0);
    R::AddSlider(dialog, &slider, 121000, 5);
    R::AddPick(dialog, &pick, 117004, choices, 2);
    const char16_t label[] = u"Move Forward", help[] = u"Walk on", value[] = u"W and Up Arrow";
    R::AddText(dialog, &text, 119001, label, help, value);
    assert(g_heap == 8);
    assert(text == 0 && R::RowCount(dialog) == 4 && g_widgets == 4);
    assert(g_rows[0].label == u"200:124001" && g_rows[0].help == u"201:124001" && g_rows[0].def == 0);
    assert((g_rows[0].texts == std::vector<std::u16string>{u"200:40300", u"200:40301"}));
    assert(g_rows[1].label == u"200:121000" && g_rows[1].def == 5);
    assert((g_rows[2].texts == std::vector<std::u16string>{u"200:117010", u"200:117011", u"200:117012"}) && g_rows[2].def == 2);
    assert(g_rows[3].label == label && g_rows[3].help == help && (g_rows[3].texts == std::vector<std::u16string>{value}));

    assert(std::u16string(R::RowLabel(dialog, 1)) == u"200:121000");
    assert(R::WritesTo(R::RowWidget(dialog, 0), &toggle) && R::WritesTo(R::RowWidget(dialog, 3), &text));
    assert(R::ValueList(R::RowWidget(dialog, 3)) == R::RowWidget(dialog, 3) + 0xa0);
    assert(R::PickChoices(R::RowWidget(dialog, 2)) == R::RowWidget(dialog, 2) + 0x100);
    assert(R::RowsList(dialog) == dialog + 0xa70 && R::IsList(R::RowsList(dialog)) && !R::IsList(dialog));
    R::SetRowLabel(dialog, 1, label);
    assert(R::RowLabel(dialog, 1) == label);
    R::Redraw(R::RowsList(dialog));
    assert(g_redraws == 1);

    assert(R::DropLastRow(dialog) && R::RowCount(dialog) == 3 && g_widgets == 3 && g_heap == 6 && g_layouts == 1);
    Put<std::int32_t>(R::RowWidget(dialog, 2) + 8, 2);
    assert(R::DropLastRow(dialog) && g_widgets == 3 && R::RowCount(dialog) == 2);
    Put(dialog + 0xe60, u64(g_records) + 1);
    assert(R::RowCount(dialog) == -1);

    R::AddCommand(nullptr, 110013, &Opener);
    R::AddExitCommand(nullptr, 110015, &Opener);
    assert((g_commands == std::vector<std::u16string>{u"200:110013", u"exit 200:110015"}) && g_opener == (void*)&Opener);
    std::uint8_t out[16];
    assert(R::AskYesNo(out, nullptr, 920001, &Yes) == out && g_yes == 1);
    assert(g_box.count == 1 && g_command.count == 1);
    assert(g_heap == 4);
    std::puts("PASS: option rows: messages, toggles, sliders, pick lists, text rows, records, dropped rows, commands");
}
