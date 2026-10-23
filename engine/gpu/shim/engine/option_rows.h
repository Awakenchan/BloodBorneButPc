// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace BbEngine::OptionRows {
void Bind(std::uint64_t image);

constexpr std::uint32_t kMenuText = 200, kLineHelp = 201, kScreenQuestion = 204;
struct Captions {
    alignas(16) std::uint8_t bytes[0x80]{};
    explicit Captions(std::uint32_t id) : Captions(id, id) {}
    Captions(std::uint32_t label, std::uint32_t help);
    ~Captions();
    Captions(const Captions&) = delete;
    Captions& operator=(const Captions&) = delete;
    void Show(const char16_t* label, const char16_t* help);
};
const char16_t* Message(std::uint32_t id, std::uint32_t group = kMenuText);

void AddSlider(void* dialog, std::uint8_t* value, std::uint32_t id, std::uint8_t def);
void AddToggle(void* dialog, std::uint8_t* value, std::uint32_t id, std::uint8_t def);
struct Choice {
    std::int32_t value;
    std::uint32_t id;
};
void AddPick(void* dialog, std::int32_t* value, std::uint32_t id, std::span<const Choice> choices, std::int32_t def);
void AddText(void* dialog, std::uint8_t* value, std::uint32_t id, const char16_t* label, const char16_t* help,
             const char16_t* text);

int RowCount(const std::uint8_t* dialog);
std::uint8_t* RowWidget(const std::uint8_t* dialog, int row);
const char16_t* RowLabel(const std::uint8_t* dialog, int row);
void SetRowLabel(std::uint8_t* dialog, int row, const char16_t* label);
std::uint8_t* RowsList(std::uint8_t* dialog);
std::uint8_t* ValueList(std::uint8_t* widget);
bool WritesTo(const std::uint8_t* widget, const void* value);
bool IsList(const std::uint8_t* list);
void Redraw(std::uint8_t* list);
void Layout(std::uint8_t* dialog);
bool DropLastRow(std::uint8_t* dialog);
void Release(std::uint64_t object);

using Handler = void(__attribute__((sysv_abi)) *)(void* dialog, void* params);
std::uint64_t OpenSection(void* root, void* params, const char* name, Handler handler);
constexpr std::uint32_t kDefaultsRow = 0x9d8a;
std::uint8_t* PickChoices(std::uint8_t* widget);

struct Function {
    alignas(16) std::uint8_t bytes[0x28]{};
    explicit Function(const void* fn, std::size_t vtable = 0x5343880);
};
using Opener = void*(__attribute__((sysv_abi)) *)(void* out, void* ctx);
void AddCommand(void* builder, std::uint32_t id, Opener opener);
void AddExitCommand(void* builder, std::uint32_t id, Opener opener);
void* OpenThrough(void* out, void* ctx, std::int64_t(__attribute__((sysv_abi)) * opener)(void*, void*));
void* AskYesNo(void* out, void* factory, std::uint32_t question, void(__attribute__((sysv_abi)) * yes)(std::int64_t, std::int64_t));
}
