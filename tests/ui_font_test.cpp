#include <cassert>
#include <vector>
#include "../src/gui/ui_font.h"
int main() {
    const UiFont* font = ui_font_for_pixels(14);
    assert(font->pixels == 14 && ui_font_for_pixels(0)->pixels == 12);
    assert(ui_font_for_pixels(1000)->pixels == 32);
    assert(ui_text_width(font, "WWW") > ui_text_width(font, "iii"));
    assert(ui_text_width(font, "") == 0);
    assert(ui_text_width(font, "\xff") == ui_text_width(font, "?"));
    assert(ui_font_blend(0xff123456, 0xffabcdef, 0) == 0xff123456);
    assert(ui_font_blend(0xff123456, 0xffabcdef, 255) == 0xffabcdef);
    constexpr int w = 170, h = 40, stride = 180;
    constexpr uint32_t bg = 0xff102030, ink = 0xffe0d0c0;
    std::vector<uint32_t> full(stride*h, bg), tiles(stride*h, bg);
    ui_text_draw(full.data(), w, h, stride, font, 5, 5, "Inter Window 123", ink, 0, 0, w, h);
    bool antialiased = false;
    for (auto pixel : full) if (pixel != bg && pixel != ink) antialiased = true;
    assert(antialiased);
    for (int y = 0; y < h; y += 3)
        for (int x = 0; x < w; x += 7)
            ui_text_draw(tiles.data(), w, h, stride, font, 5, 5, "Inter Window 123", ink, x, y, x+7, y+3);
    assert(full == tiles); // damage boundaries must not alter antialiased glyphs
    std::vector<uint32_t> clipped(stride*h, bg);
    ui_text_draw(clipped.data(), w, h, stride, font, -3, -3, "Window", ink, 10, 7, 70, 25);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < stride; ++x)
            if (x < 10 || x >= 70 || y < 7 || y >= 25) assert(clipped[y*stride+x] == bg);
    for (const UiFont& size : ui_fonts) {
        assert(size.height >= size.pixels && size.ascent <= size.height);
        for (unsigned i = 0; i < 95; ++i) assert(size.glyphs[i].advance > 0);
    }
}
