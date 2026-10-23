// SPDX-License-Identifier: GPL-3.0-or-later
#include "xml.h"

#include <expat.h>

namespace start {

std::optional<std::string> XmlElement::Get(std::string_view key) const {
    for (const auto& [k, v] : attributes)
        if (k == key) return v;
    return std::nullopt;
}

std::string XmlElement::Get(std::string_view key, std::string_view fallback) const {
    const auto value = Get(key);
    return value ? *value : std::string(fallback);
}

void XmlElement::Iter(std::string_view tag, std::vector<const XmlElement*>& out) const {
    if (name == tag) out.push_back(this);
    for (const auto& child : children) child->Iter(tag, out);
}

namespace {
struct Builder {
    std::unique_ptr<XmlElement> root;
    std::vector<XmlElement*> stack;
};

void XMLCALL OnStart(void* data, const XML_Char* name, const XML_Char** attributes) {
    auto* b = static_cast<Builder*>(data);
    auto element = std::make_unique<XmlElement>();
    element->name = name;
    for (int i = 0; attributes[i]; i += 2) element->attributes.emplace_back(attributes[i], attributes[i + 1]);
    XmlElement* raw = element.get();
    if (b->stack.empty()) b->root = std::move(element);
    else b->stack.back()->children.push_back(std::move(element));
    b->stack.push_back(raw);
}

void XMLCALL OnEnd(void* data, const XML_Char*) {
    static_cast<Builder*>(data)->stack.pop_back();
}

void XMLCALL OnText(void* data, const XML_Char* text, int length) {
    auto* b = static_cast<Builder*>(data);
    if (!b->stack.empty()) b->stack.back()->text.append(text, std::size_t(length));
}
}

std::unique_ptr<XmlElement> ParseXml(const fs::path& path) {
    const Bytes data = ReadFile(path);
    Builder builder;
    XML_Parser parser = XML_ParserCreate(nullptr);
    XML_SetUserData(parser, &builder);
    XML_SetElementHandler(parser, OnStart, OnEnd);
    XML_SetCharacterDataHandler(parser, OnText);
    const bool ok = XML_Parse(parser, reinterpret_cast<const char*>(data.data()), int(data.size()), XML_TRUE) != XML_STATUS_ERROR;
    std::string message;
    if (!ok)
        message = std::string(XML_ErrorString(XML_GetErrorCode(parser))) + ": line " +
                  std::to_string(XML_GetCurrentLineNumber(parser)) + ", column " +
                  std::to_string(XML_GetCurrentColumnNumber(parser));
    XML_ParserFree(parser);
    if (!ok || !builder.root) throw Error(ok ? "empty XML document" : message);
    return std::move(builder.root);
}

}
