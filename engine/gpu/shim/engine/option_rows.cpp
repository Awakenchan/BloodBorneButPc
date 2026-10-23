// SPDX-License-Identifier: GPL-3.0-or-later
#include "option_rows.h"
#include "engine_hooks.h"
#include <cstring>

namespace BbEngine::OptionRows {
namespace {
constexpr std::size_t kMessage = 0x1ae8cc0;
constexpr std::size_t kStringFrom = 0x2967040;

constexpr std::size_t kSlider = 0x1b2ac00;
constexpr std::size_t kChoice = 0x1b2a100;
constexpr std::size_t kPick = 0x1b29370;
constexpr std::size_t kOnOff = 0x1b2b3b0;
constexpr std::size_t kChoiceAppend = 0x1b2c200, kChoiceCount = 0x98, kChoiceListBytes = 0xa0;
constexpr std::size_t kPickAppend = 0x1b1c0c0, kPickCount = 0x908, kPickListBytes = 0x910, kPickMost = 32;
constexpr std::size_t kEntryBytes = 0x48, kEntryText = 0x08, kEntryString = 0x10, kEntryFlag = 0x40;

constexpr std::size_t kRecordsBegin = 0xe58, kRecordsEnd = 0xe60, kRecordBytes = 0x90;
constexpr std::size_t kRecordLabel = 0x00, kRecordHelp = 0x40, kRecordWidget = 0x80;
constexpr std::size_t kRows = 0xa70, kListVtable = 0x5399fb0;
constexpr std::size_t kChoiceList = 0xa0, kChoiceValue = 0x360, kPickChoices = 0x100;
constexpr std::size_t kRedraw = 0x1ad06e0;
constexpr std::size_t kLayout = 0x1b28de0;

constexpr std::size_t kOpenSection = 0x1b20900;
constexpr std::size_t kAddCommand = 0x1b4e2a0;
constexpr std::size_t kAddExit = 0x1b4c9a0;
constexpr std::size_t kOpenThrough = 0x1bb4c70;
constexpr std::size_t kCommandFunctor = 0x533d120;
constexpr std::size_t kQuestionBox = 0x1c1ed10, kCommand = 0x195db60, kBoxWithCommand = 0x1c1a070;

std::uint64_t g_image = 0;
std::uint64_t At(std::size_t offset) { return g_image + offset; }

template <typename T> T Get(const void* at) {
    T v{};
    std::memcpy(&v, at, sizeof v);
    return v;
}
template <typename T> void Put(void* at, T v) { std::memcpy(at, &v, sizeof v); }
std::uint8_t* Bytes(std::uint64_t address) { return reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(address)); }

void FreeString(std::uint8_t* s) {
    if (Get<std::uint64_t>(s + 0x20) >= 8) {
        const auto text = Get<std::uint64_t>(s + 0x08), allocator = Get<std::uint64_t>(s + 0x28);
        if (text && allocator) Call(Get<std::uint64_t>(Bytes(Get<std::uint64_t>(Bytes(allocator)) + 0x70)), allocator, text);
    }
    Put<std::uint64_t>(s + 0x20, 7);
    Put<std::uint64_t>(s + 0x18, 0);
    Put<std::uint16_t>(s + 0x08, 0);
}

void MakeEntry(std::uint8_t* entry, std::int32_t value, const char16_t* text) {
    if (!text) text = u"";
    std::memset(entry, 0, kEntryBytes);
    Put(entry, value);
    Put(entry + kEntryText, text);
    Call(At(kStringFrom), entry + kEntryString, text);
    entry[kEntryFlag] = 1;
}
void FreeEntries(std::uint8_t* list, std::uint64_t count) {
    for (std::uint64_t i = 0; i < count; ++i) FreeString(list + i * kEntryBytes + kEntryString);
}

std::uint8_t* Record(const std::uint8_t* dialog, int row) {
    return Bytes(Get<std::uint64_t>(dialog + kRecordsBegin) + std::uint64_t(row) * kRecordBytes);
}
}

void Bind(std::uint64_t image) { g_image = image; }

Captions::Captions(std::uint32_t label, std::uint32_t help) {
    Call(At(kMessage), bytes, kMenuText, label);
    Call(At(kMessage), bytes + 0x40, kLineHelp, help);
}
Captions::~Captions() {
    FreeString(bytes + 0x08);
    FreeString(bytes + 0x48);
}
void Captions::Show(const char16_t* label, const char16_t* help) {
    Put(bytes, label);
    Put(bytes + 0x40, help);
}

const char16_t* Message(std::uint32_t id, std::uint32_t group) {
    alignas(16) std::uint8_t m[0x40]{};
    Call(At(kMessage), m, group, id);
    const auto text = Get<const char16_t*>(m);
    FreeString(m + 0x08);
    return text && *text ? text : nullptr;
}

void AddSlider(void* dialog, std::uint8_t* value, std::uint32_t id, std::uint8_t def) {
    Captions c(id);
    Call(At(kSlider), dialog, c.bytes, value, &def);
}

void AddToggle(void* dialog, std::uint8_t* value, std::uint32_t id, std::uint8_t def) {
    Captions c(id);
    alignas(16) std::uint8_t list[kChoiceListBytes]{};
    Call(At(kOnOff), list);
    Call(At(kChoice), dialog, c.bytes, value, list, &def);
    FreeEntries(list, Get<std::uint64_t>(list + kChoiceCount));
}

void AddPick(void* dialog, std::int32_t* value, std::uint32_t id, std::span<const Choice> choices, std::int32_t def) {
    Captions c(id);
    alignas(16) std::uint8_t list[kPickListBytes]{};
    const std::size_t n = choices.size() < kPickMost ? choices.size() : kPickMost;
    for (std::size_t i = 0; i < n; ++i) {
        alignas(16) std::uint8_t entry[kEntryBytes];
        MakeEntry(entry, choices[i].value, Message(choices[i].id));
        Call(At(kPickAppend), list, entry);
        FreeEntries(entry, 1);
    }
    Call(At(kPick), dialog, c.bytes, value, list, &def);
    FreeEntries(list, Get<std::uint64_t>(list + kPickCount));
}

void AddText(void* dialog, std::uint8_t* value, std::uint32_t id, const char16_t* label, const char16_t* help,
             const char16_t* text) {
    Captions c(id);
    c.Show(label, help);
    alignas(16) std::uint8_t list[kChoiceListBytes]{};
    alignas(16) std::uint8_t entry[kEntryBytes];
    MakeEntry(entry, 0, text);
    Call(At(kChoiceAppend), list, entry);
    FreeEntries(entry, 1);
    *value = 0;
    std::uint8_t def = 0;
    Call(At(kChoice), dialog, c.bytes, value, list, &def);
    FreeEntries(list, Get<std::uint64_t>(list + kChoiceCount));
}

int RowCount(const std::uint8_t* dialog) {
    const auto begin = Get<std::uint64_t>(dialog + kRecordsBegin), end = Get<std::uint64_t>(dialog + kRecordsEnd);
    if (!begin || end < begin || (end - begin) % kRecordBytes) return -1;
    return int((end - begin) / kRecordBytes);
}
std::uint8_t* RowWidget(const std::uint8_t* dialog, int row) { return Bytes(Get<std::uint64_t>(Record(dialog, row) + kRecordWidget)); }
const char16_t* RowLabel(const std::uint8_t* dialog, int row) { return Get<const char16_t*>(Record(dialog, row) + kRecordLabel); }
void SetRowLabel(std::uint8_t* dialog, int row, const char16_t* label) { Put(Record(dialog, row) + kRecordLabel, label); }
std::uint8_t* RowsList(std::uint8_t* dialog) { return dialog + kRows; }
std::uint8_t* ValueList(std::uint8_t* widget) { return widget + kChoiceList; }
std::uint8_t* PickChoices(std::uint8_t* widget) { return widget + kPickChoices; }
bool WritesTo(const std::uint8_t* widget, const void* value) { return widget && Get<const void*>(widget + kChoiceValue) == value; }
bool IsList(const std::uint8_t* list) { return list && Get<std::uint64_t>(list) == At(kListVtable); }
void Redraw(std::uint8_t* list) { Call(At(kRedraw), list); }
void Layout(std::uint8_t* dialog) { Call(At(kLayout), dialog, 0); }

void Release(std::uint64_t object) {
    if (!object) return;
    if (__atomic_fetch_sub(reinterpret_cast<std::int32_t*>(Bytes(object + 8)), 1, __ATOMIC_ACQ_REL) == 1)
        Call(Get<std::uint64_t>(Bytes(Get<std::uint64_t>(Bytes(object)))), object);
}

bool DropLastRow(std::uint8_t* dialog) {
    const int rows = RowCount(dialog);
    if (rows < 1) return false;
    std::uint8_t* last = Record(dialog, rows - 1);
    FreeString(last + kRecordLabel + 0x08);
    FreeString(last + kRecordHelp + 0x08);
    Release(Get<std::uint64_t>(last + kRecordWidget));
    Put(dialog + kRecordsEnd, Get<std::uint64_t>(dialog + kRecordsEnd) - kRecordBytes);
    Layout(dialog);
    return true;
}

std::uint64_t OpenSection(void* root, void* params, const char* name, Handler handler) {
    return std::uint64_t(Call(At(kOpenSection), root, params, name, handler, 0, 0));
}

Function::Function(const void* fn, std::size_t vtable) {
    Put(bytes, At(vtable));
    Put(bytes + 0x08, fn);
    Put(bytes + 0x20, static_cast<void*>(bytes));
}

void AddCommand(void* builder, std::uint32_t id, Opener opener) {
    Captions c(id);
    Function f(reinterpret_cast<const void*>(opener), kCommandFunctor);
    std::uint64_t flag = 0;
    Call(At(kAddCommand), builder, c.bytes, f.bytes, &flag);
}

void AddExitCommand(void* builder, std::uint32_t id, Opener opener) {
    Captions c(id);
    Function f(reinterpret_cast<const void*>(opener), kCommandFunctor);
    Call(At(kAddExit), builder, c.bytes, f.bytes);
}

void* OpenThrough(void* out, void* ctx, std::int64_t(__attribute__((sysv_abi)) * opener)(void*, void*)) {
    Function f(reinterpret_cast<const void*>(opener));
    Call(At(kOpenThrough), out, ctx, f.bytes);
    return out;
}

void* AskYesNo(void* out, void* factory, std::uint32_t question, void(__attribute__((sysv_abi)) * yes)(std::int64_t, std::int64_t)) {
    alignas(16) std::uint8_t m[0x40]{};
    Call(At(kMessage), m, kScreenQuestion, question);
    std::uint64_t box = 0, command = 0;
    Call(At(kQuestionBox), &box, factory, m);
    Function f(reinterpret_cast<const void*>(yes));
    Call(At(kCommand), &command, f.bytes, 2);
    Call(At(kBoxWithCommand), out, &box, &command);
    Release(command);
    Release(box);
    FreeString(m + 0x08);
    return out;
}
}
