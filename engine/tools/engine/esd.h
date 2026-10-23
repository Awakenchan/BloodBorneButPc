// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace esd {

using Bytes = std::vector<std::uint8_t>;
using Ref = std::int64_t;
constexpr Ref kNone = -1;
constexpr Ref kNew = std::int64_t(1) << 48;

struct List {
    Ref at = kNone;
    std::int64_t count = 0;
};

struct Command {
    std::int32_t bank = 1, id = 0;
    std::vector<Bytes> args;
};

class Script {
public:
    static std::optional<Script> Read(Bytes bytes, std::string* why);
    Bytes Write() const;

    Ref State(std::int64_t group, std::int64_t id) const;
    List StateList(Ref state, int which) const;
    std::vector<Ref> Conditions(List list) const;
    std::vector<Ref> Commands(List list) const;

    Ref NewState(std::int64_t group, std::int64_t id);
    Ref NewCondition(Ref target, const Bytes& expression, List pass = {});
    List NewConditions(const std::vector<Ref>& conditions);
    List NewCommands(const std::vector<Command>& commands, const std::vector<Ref>& then = {});
    void SetStateList(Ref state, int which, List list);

private:
    struct Chunk {
        int kind;
        Ref before;
        Bytes bytes;
    };
    Bytes data_;
    std::int64_t section_[8]{};
    std::int64_t count_[6]{};
    std::vector<Chunk> chunks_;
    struct Patch {
        std::int64_t at, value;
        bool ref;
    };
    std::vector<Patch> patches_;

    std::int64_t Q(std::int64_t at) const;
    std::int64_t Field(Ref element, std::int64_t at) const;
    Ref Add(int kind, Ref before, Bytes bytes);
    void Set(Ref element, std::int64_t at, std::int64_t value, bool ref);
};

Bytes Int(std::int64_t value);
Bytes Real(double value);
Bytes Call(int function, const std::vector<std::int64_t>& args, bool keep = false);
Bytes Register0();
Bytes End(Bytes code);
Bytes Equals(Bytes lhs, std::int64_t rhs);
Bytes True();

}
