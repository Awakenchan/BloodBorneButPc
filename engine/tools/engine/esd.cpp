// SPDX-License-Identifier: GPL-3.0-or-later
#include "esd.h"

#include <algorithm>
#include <cstring>
#include <numeric>

namespace esd {
namespace {

constexpr std::size_t kHeader = 0x6c;
enum Kind { kGroups, kStates, kConditions, kCommands, kArguments, kLists, kExpressions, kName };
constexpr std::int64_t kSize[] = {0x20, 0x48, 0x38, 0x18, 0x10, 8};
const std::vector<std::int64_t> kOffsets[] = {
    {8, 24}, {8, 24, 40, 56}, {0, 8, 24, 40}, {8}, {0}, {0},
};
constexpr std::size_t kCountAt[] = {0, 0x30, 0x38, 0x40, 0x48, 0x50};

std::int32_t I32(const Bytes& b, std::size_t at) {
    std::int32_t v;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}
void Put32(Bytes& b, std::size_t at, std::int64_t v) {
    const auto w = static_cast<std::int32_t>(v);
    std::memcpy(b.data() + at, &w, 4);
}
std::int64_t Get64(const Bytes& b, std::size_t at) {
    std::int64_t v;
    std::memcpy(&v, b.data() + at, 8);
    return v;
}
void Put64(Bytes& b, std::size_t at, std::int64_t v) { std::memcpy(b.data() + at, &v, 8); }

}

std::int64_t Script::Q(std::int64_t at) const { return Get64(data_, kHeader + static_cast<std::size_t>(at)); }

std::optional<Script> Script::Read(Bytes bytes, std::string* why) {
    const auto fail = [&](const char* reason) -> std::optional<Script> {
        if (why) *why = reason;
        return std::nullopt;
    };
    if (bytes.size() < kHeader + 0x48 || std::memcmp(bytes.data(), "fsSL", 4) != 0)
        return fail("not a 64-bit talk script");
    Script s;
    s.data_ = std::move(bytes);
    const Bytes& d = s.data_;
    const std::int64_t size = I32(d, 0x14);
    if (size < 0x48 || std::uint64_t(size) + kHeader != d.size() || I32(d, 0x5c) != size)
        return fail("the talk script's size is not its header's");
    s.count_[kGroups] = s.Q(0x20);
    for (int k = kStates; k <= kLists; ++k) s.count_[k] = I32(d, kCountAt[k]);
    const std::int64_t groups = s.Q(0x18);
    if (groups != 0x48 && !(groups == kNone && s.count_[kGroups] == 0)) return fail("the state groups are not where they belong");
    s.section_[kGroups] = 0x48;
    for (int k = kGroups; k < kExpressions; ++k) {
        if (s.count_[k] < 0 || s.count_[k] > size / kSize[k]) return fail("an implausible element count");
        s.section_[k + 1] = s.section_[k] + kSize[k] * s.count_[k];
    }
    s.section_[kName] = I32(d, 0x54);
    const std::int64_t name = s.Q(0x28), name_length = s.Q(0x30);
    if (s.section_[kLists] != I32(d, 0x4c) ||
        s.section_[kExpressions] > s.section_[kName] || name < s.section_[kName] || name > s.section_[kName] + 1 ||
        name_length != I32(d, 0x58) || name + 2 * name_length != size)
        return fail("the talk script's arrays do not tile its data block");
    std::int64_t states = 0;
    for (std::int64_t g = 0; g < s.count_[kGroups]; ++g) {
        const std::int64_t at = s.section_[kGroups] + kSize[kGroups] * g, first = s.Q(at + 8), n = s.Q(at + 16);
        if (first != s.section_[kStates] + kSize[kStates] * states || s.Q(at + 24) != first || n < 1)
            return fail("a state group that is not its own run of states");
        states += n + 1;
    }
    if (states != s.count_[kStates]) return fail("the groups do not account for every state");
    const auto inside = [&](std::int64_t at, std::int64_t n, int kind) {
        return n == 0 || (at >= s.section_[kind] && at + n * (kind == kExpressions ? 1 : kSize[kind]) <= s.section_[kind + 1]);
    };
    for (std::int64_t k = 0; k < s.count_[kStates]; ++k) {
        const std::int64_t at = s.section_[kStates] + kSize[kStates] * k;
        if (!inside(s.Q(at + 8), s.Q(at + 16), kLists) || !inside(s.Q(at + 24), s.Q(at + 32), kCommands) ||
            !inside(s.Q(at + 40), s.Q(at + 48), kCommands) || !inside(s.Q(at + 56), s.Q(at + 64), kCommands))
            return fail("a state's list outside its array");
    }
    for (std::int64_t k = 0; k < s.count_[kConditions]; ++k) {
        const std::int64_t at = s.section_[kConditions] + kSize[kConditions] * k, target = s.Q(at);
        if ((target != kNone && !inside(target, 1, kStates)) || !inside(s.Q(at + 8), s.Q(at + 16), kCommands) ||
            !inside(s.Q(at + 24), s.Q(at + 32), kLists) || !inside(s.Q(at + 40), s.Q(at + 48), kExpressions))
            return fail("a condition's element outside its array");
    }
    for (std::int64_t k = 0; k < s.count_[kCommands]; ++k) {
        const std::int64_t at = s.section_[kCommands] + kSize[kCommands] * k;
        if (!inside(s.Q(at + 8), s.Q(at + 16), kArguments)) return fail("a command's arguments outside their array");
    }
    for (std::int64_t k = 0; k < s.count_[kArguments]; ++k) {
        const std::int64_t at = s.section_[kArguments] + kSize[kArguments] * k;
        if (!inside(s.Q(at), s.Q(at + 8), kExpressions)) return fail("an argument outside the expressions");
    }
    for (std::int64_t k = 0; k < s.count_[kLists]; ++k)
        if (!inside(s.Q(s.section_[kLists] + 8 * k), 1, kConditions)) return fail("a condition list entry outside the conditions");
    return s;
}

Ref Script::State(std::int64_t group, std::int64_t id) const {
    for (std::int64_t g = 0; g < count_[kGroups]; ++g) {
        const std::int64_t at = section_[kGroups] + kSize[kGroups] * g;
        if (Q(at) != group) continue;
        for (std::int64_t k = 0; k < Q(at + 16); ++k) {
            const std::int64_t state = Q(at + 8) + kSize[kStates] * k;
            if (Q(state) == id) return state;
        }
    }
    return kNone;
}

std::int64_t Script::Field(Ref element, std::int64_t at) const {
    if (element < kNew) {
        for (const Patch& p : patches_)
            if (p.at == element + at) return p.value;
        return Q(element + at);
    }
    return Get64(chunks_[static_cast<std::size_t>(element - kNew)].bytes, static_cast<std::size_t>(at));
}

void Script::Set(Ref element, std::int64_t at, std::int64_t value, bool ref) {
    if (element >= kNew) {
        Put64(chunks_[static_cast<std::size_t>(element - kNew)].bytes, static_cast<std::size_t>(at), value);
        return;
    }
    for (Patch& p : patches_)
        if (p.at == element + at) {
            p.value = value;
            return;
        }
    patches_.push_back({element + at, value, ref});
}

List Script::StateList(Ref state, int which) const {
    return {Field(state, 8 + 16 * which), Field(state, 16 + 16 * which)};
}

std::vector<Ref> Script::Conditions(List list) const {
    std::vector<Ref> out;
    for (std::int64_t k = 0; k < list.count; ++k) out.push_back(Field(list.at, 8 * k));
    return out;
}

std::vector<Ref> Script::Commands(List list) const {
    std::vector<Ref> out;
    for (std::int64_t k = 0; k < list.count && list.at < kNew; ++k) out.push_back(list.at + kSize[kCommands] * k);
    return out;
}

Ref Script::Add(int kind, Ref before, Bytes bytes) {
    chunks_.push_back({kind, before, std::move(bytes)});
    return kNew + static_cast<Ref>(chunks_.size() - 1);
}

Ref Script::NewState(std::int64_t group, std::int64_t id) {
    for (std::int64_t g = 0; g < count_[kGroups]; ++g) {
        const std::int64_t at = section_[kGroups] + kSize[kGroups] * g;
        if (Q(at) != group) continue;
        Bytes state(kSize[kStates], 0);
        Put64(state, 0, id);
        for (int k = 0; k < 4; ++k) Put64(state, 8 + 16 * k, kNone);
        const Ref ref = Add(kStates, Q(at + 8) + kSize[kStates] * Q(at + 16), std::move(state));
        Set(at, 16, Field(at, 16) + 1, false);
        return ref;
    }
    return kNone;
}

Ref Script::NewCondition(Ref target, const Bytes& expression, List pass) {
    const Ref code = Add(kExpressions, section_[kName], expression);
    Bytes c(kSize[kConditions], 0);
    Put64(c, 0, target);
    Put64(c, 8, pass.count ? pass.at : kNone);
    Put64(c, 16, pass.count);
    Put64(c, 24, kNone);
    Put64(c, 40, code);
    Put64(c, 48, static_cast<std::int64_t>(expression.size()));
    return Add(kConditions, section_[kCommands], std::move(c));
}

List Script::NewConditions(const std::vector<Ref>& conditions) {
    Bytes list(8 * conditions.size());
    for (std::size_t k = 0; k < conditions.size(); ++k) Put64(list, 8 * k, conditions[k]);
    return {Add(kLists, section_[kExpressions], std::move(list)), static_cast<std::int64_t>(conditions.size())};
}

List Script::NewCommands(const std::vector<Command>& commands, const std::vector<Ref>& then) {
    Bytes list;
    for (const Command& c : commands) {
        Bytes args;
        for (const Bytes& a : c.args) {
            Bytes arg(kSize[kArguments], 0);
            Put64(arg, 0, Add(kExpressions, section_[kName], a));
            Put64(arg, 8, static_cast<std::int64_t>(a.size()));
            args.insert(args.end(), arg.begin(), arg.end());
        }
        Bytes cmd(kSize[kCommands], 0);
        Put32(cmd, 0, c.bank);
        Put32(cmd, 4, c.id);
        Put64(cmd, 8, args.empty() ? kNone : Add(kArguments, section_[kLists], std::move(args)));
        Put64(cmd, 16, static_cast<std::int64_t>(c.args.size()));
        list.insert(list.end(), cmd.begin(), cmd.end());
    }
    for (const Ref old : then) {
        const auto from = data_.begin() + static_cast<std::ptrdiff_t>(kHeader + old);
        list.insert(list.end(), from, from + kSize[kCommands]);
    }
    const auto count = static_cast<std::int64_t>(list.size()) / kSize[kCommands];
    return {Add(kCommands, section_[kArguments], std::move(list)), count};
}

void Script::SetStateList(Ref state, int which, List list) {
    Set(state, 8 + 16 * which, list.count ? list.at : kNone, true);
    Set(state, 16 + 16 * which, list.count, false);
}

Bytes Script::Write() const {
    std::vector<std::size_t> order(chunks_.size());
    std::iota(order.begin(), order.end(), std::size_t(0));
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return chunks_[a].before != chunks_[b].before ? chunks_[a].before < chunks_[b].before
                                                      : chunks_[a].kind < chunks_[b].kind;
    });
    const auto moved = [&](std::int64_t at) {
        std::int64_t by = 0;
        for (const Chunk& c : chunks_)
            if (c.before <= at) by += static_cast<std::int64_t>(c.bytes.size());
        return at + by;
    };
    std::vector<std::int64_t> placed(chunks_.size());
    Bytes block;
    std::int64_t from = 0;
    for (const std::size_t k : order) {
        const Chunk& c = chunks_[k];
        block.insert(block.end(), data_.begin() + static_cast<std::ptrdiff_t>(kHeader + from),
                     data_.begin() + static_cast<std::ptrdiff_t>(kHeader + c.before));
        from = c.before;
        placed[k] = static_cast<std::int64_t>(block.size());
        block.insert(block.end(), c.bytes.begin(), c.bytes.end());
    }
    block.insert(block.end(), data_.begin() + static_cast<std::ptrdiff_t>(kHeader + from),
                 data_.begin() + static_cast<std::ptrdiff_t>(kHeader + section_[kName]));
    const std::int64_t name_block = static_cast<std::int64_t>(block.size());
    if (block.size() & 1) block.push_back(0);
    const std::int64_t name = static_cast<std::int64_t>(block.size());
    block.insert(block.end(), data_.begin() + static_cast<std::ptrdiff_t>(kHeader + Q(0x28)), data_.end());

    const auto resolve = [&](Ref r) {
        return r == kNone ? kNone : r >= kNew ? placed[static_cast<std::size_t>(r - kNew)] : moved(r);
    };
    for (int kind = kGroups; kind < kExpressions; ++kind)
        for (std::int64_t k = 0; k < count_[kind]; ++k) {
            const std::int64_t at = section_[kind] + kSize[kind] * k;
            for (const std::int64_t f : kOffsets[kind]) {
                std::int64_t v = Q(at + f);
                for (const Patch& p : patches_)
                    if (p.at == at + f) v = p.value;
                Put64(block, static_cast<std::size_t>(moved(at + f)), resolve(v));
            }
        }
    for (const Patch& p : patches_)
        if (!p.ref) Put64(block, static_cast<std::size_t>(moved(p.at)), p.value);
    for (std::size_t k = 0; k < chunks_.size(); ++k) {
        const Chunk& c = chunks_[k];
        if (c.kind == kExpressions) continue;
        for (std::int64_t e = 0; e < static_cast<std::int64_t>(c.bytes.size()); e += kSize[c.kind])
            for (const std::int64_t f : kOffsets[c.kind]) {
                const auto at = static_cast<std::size_t>(placed[k] + e + f);
                Put64(block, at, resolve(Get64(block, at)));
            }
    }
    Put64(block, 0x28, name);

    Bytes out(data_.begin(), data_.begin() + kHeader);
    std::int64_t count[6];
    std::copy(std::begin(count_), std::end(count_), count);
    for (const Chunk& c : chunks_)
        if (c.kind != kExpressions) count[c.kind] += static_cast<std::int64_t>(c.bytes.size()) / kSize[c.kind];
    const auto size = static_cast<std::int64_t>(block.size());
    for (int k = kStates; k <= kLists; ++k) Put32(out, kCountAt[k], count[k]);
    Put32(out, 0x14, size);
    Put32(out, 0x4c, moved(section_[kLists]));
    Put32(out, 0x54, name_block);
    Put32(out, 0x5c, size);
    Put32(out, 0x64, size);
    out.insert(out.end(), block.begin(), block.end());
    return out;
}

Bytes Int(std::int64_t value) {
    if (value >= -64 && value <= 63) return {static_cast<std::uint8_t>(0x40 + value)};
    Bytes out{0x82, 0, 0, 0, 0};
    Put32(out, 1, value);
    return out;
}

Bytes Real(double value) {
    Bytes out(9, 0x81);
    std::memcpy(out.data() + 1, &value, 8);
    return out;
}

Bytes Call(int function, const std::vector<std::int64_t>& args, bool keep) {
    Bytes out = Int(function);
    for (const std::int64_t a : args) {
        const Bytes v = Int(a);
        out.insert(out.end(), v.begin(), v.end());
    }
    out.push_back(static_cast<std::uint8_t>(0x84 + args.size()));
    out.push_back(keep ? 0xa7 : 0xa6);
    return out;
}

Bytes Register0() { return {0xaf}; }

Bytes End(Bytes code) {
    code.push_back(0xa1);
    return code;
}

Bytes Equals(Bytes lhs, std::int64_t rhs) {
    const Bytes v = Int(rhs);
    lhs.insert(lhs.end(), v.begin(), v.end());
    lhs.push_back(0x95);
    return End(std::move(lhs));
}

Bytes True() { return End(Int(1)); }

}
