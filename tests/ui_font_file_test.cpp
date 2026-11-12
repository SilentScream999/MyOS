#include <cassert>
#include <fstream>
#include <iterator>
#include <vector>
#include "../src/gui/ui_font_file.h"

int main(int argc, char** argv) {
    assert(argc == 2);
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    UiFontPack pack{};
    assert(ui_font_parse(bytes.data(), bytes.size(), &pack));
    assert(pack.family.count == 8);
    ui_active_family = &pack.family;
    assert(ui_font_for_pixels(0)->pixels == 12);
    assert(ui_font_for_pixels(15)->pixels == 14);
    assert(ui_font_for_pixels(1000)->pixels == 32);
    auto* font = ui_font_for_pixels(14);
    assert(ui_text_width(font, "WWW") > ui_text_width(font, "iii"));
    constexpr uint32_t bg = 0xff203040;
    std::vector<uint32_t> full(240 * 60, bg), tiled = full;
    ui_text_draw(full.data(), 240, 60, 240, font, 2, 2, "Swappable font 123", 0xffffffff, 0, 0, 240, 60);
    for (int y = 0; y < 60; y += 5)
        for (int x = 0; x < 240; x += 8)
            ui_text_draw(tiled.data(), 240, 60, 240, font, 2, 2, "Swappable font 123", 0xffffffff, x, y, x+8, y+5);
    assert(full == tiled && full != std::vector<uint32_t>(full.size(), bg));
    // Every truncation must fail, preserving the previously loaded family.
    for (size_t length = 0; length < bytes.size(); ++length) {
        assert(!ui_font_parse(bytes.data(), length, &pack));
        assert(pack.family.count == 8 && ui_font_for_pixels(14) == font);
    }
    auto reject = [&](size_t offset, uint8_t value) {
        auto copy = bytes;
        copy[offset] = value;
        assert(!ui_font_parse(copy.data(), copy.size(), &pack));
    };
    reject(0, 0); // magic
    reject(8, 0); reject(8, 17); reject(12, 1); // count/reserved
    reject(16, 0); reject(18, 255); reject(19, 1); // metrics/reserved
    reject(32, 12); // duplicate size
    auto glyphs = ui_font_u32(bytes.data() + 20);
    reject(glyphs + 8, 0); // zero advance
    reject(glyphs + 9, 1); // reserved glyph bytes
    auto bad = bytes;
    for (int i = 0; i < 4; ++i) bad[glyphs+i] = 255; // overflowing coverage offset
    assert(!ui_font_parse(bad.data(), bad.size(), &pack));
    assert(!ui_font_parse(nullptr, bytes.size(), &pack));
    assert(!ui_font_parse(bytes.data(), bytes.size(), nullptr));
    assert(!ui_font_parse(bytes.data(), UI_FONT_FILE_MAX + 1, &pack));
    ui_active_family = &ui_builtin_family;
}
