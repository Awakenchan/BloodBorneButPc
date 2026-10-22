// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic message bundles only: no game files required.
#define main menu_assets_program_main
#include "../tools/engine/menu_assets.cpp"
#undef main
#include <cassert>

int main(int argc, char** argv) {
    const auto english = read_messages();
    const auto chinese = read_messages(true);
    assert(english.size() == chinese.size());
    assert(english.at(110010).text == "PC Effects");
    assert(chinese.at(110010).text == "PC 特效");
    assert(chinese.at(110015).help == "关闭游戏并返回桌面。");
    assert(chinese.at(117010).text == "1280x720"); // untranslated technical value
    assert(chinese.at(920000).text.find('\n') != std::string::npos);
    assert(chinese.at(119100).text == "向前移动");
    assert(chinese.at(119129).text == "调试菜单");
    assert(chinese.at(119204).text == "其他");
    assert(chinese.at(119215).text.front() == ' ');

    Bnd4 original;
    original.head.resize(0x40, 0);
    std::memcpy(original.head.data(), "BND4", 4);
    original.esize = 0x24;
    original.unicode = true;
    original.head[0x30] = 1;
    put64(original.head, 0x20, original.esize);
    const auto sentinel = utf16("原有文字 한글 😀");
    for (const auto group : {200u, 201u, 78u, 204u, 30u, 999u}) {
        BndFile file{};
        file.id = group;
        file.name = utf16("测试.fmg");
        file.data = fmg_write(Fmg{{42, sentinel}, {43, std::nullopt}});
        original.files.push_back(file);
    }
    put32(original.head, 0x0c, original.files.size());
    const auto raw = bnd_pack(original);
    if (argc == 2) {
        std::ofstream fixture(argv[1], std::ios::binary);
        fixture.write(reinterpret_cast<const char*>(raw.data()), raw.size());
        assert(fixture.good());
    }
    for (const bool zh : {false, true}) {
        const auto output = bnd_read(build_menu_messages(raw, zh));
        assert(output.files.size() == original.files.size());
        for (std::size_t i = 0; i < output.files.size(); ++i) {
            const auto& file = output.files[i];
            assert(file.id == original.files[i].id && file.name == original.files[i].name);
            if (file.id == 999) { assert(file.data == original.files[i].data); continue; }
            const auto messages = fmg_read(file.data);
            assert(messages.at(42) == sentinel && !messages.at(43));
            const auto& expected = zh ? chinese : english;
            for (const auto& [id, message] : expected) {
                const auto kind = file.id == 30 ? 2 : (file.id == 78 || file.id == 204) ? 1 : 0;
                if (message_kind(id) != kind) { assert(!messages.count(id)); continue; }
                const auto& text = file.id == 201 ? message.help : message.text;
                assert(messages.at(id) == (text.empty() ? std::nullopt : std::optional<Bytes>(utf16(text))));
            }
        }
    }
    std::puts("PASS: Chinese and English native menu messages, fallback, newlines, dynamic captions, Unicode and existing groups preserved");
}
