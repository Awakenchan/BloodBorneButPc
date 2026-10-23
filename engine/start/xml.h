// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common.h"

#include <memory>

namespace start {

struct XmlElement {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::string text;
    std::vector<std::unique_ptr<XmlElement>> children;

    std::optional<std::string> Get(std::string_view key) const;
    std::string Get(std::string_view key, std::string_view fallback) const;
    void Iter(std::string_view tag, std::vector<const XmlElement*>& out) const;
    std::vector<const XmlElement*> Iter(std::string_view tag) const {
        std::vector<const XmlElement*> out;
        Iter(tag, out);
        return out;
    }
};

std::unique_ptr<XmlElement> ParseXml(const fs::path& path);

}
