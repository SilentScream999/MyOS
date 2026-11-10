#ifndef WYZAR_UI_FONT_H
#define WYZAR_UI_FONT_H
#include <stdint.h>

struct UiGlyph {
    uint32_t offset;
    uint8_t width, height;
    int8_t left, top;
    uint8_t advance;
};
struct UiFont {
    uint8_t pixels, height, ascent;
    const UiGlyph* glyphs;
    const uint8_t* coverage;
};
#include "ui_font_data.h"
static const UiFont* ui_active_fonts = ui_fonts;
static unsigned ui_active_font_count = sizeof(ui_fonts) / sizeof(ui_fonts[0]);

static const UiFont* ui_font_for_pixels(uint32_t pixels) {
    const UiFont* chosen = &ui_active_fonts[0];
    if (pixels <= chosen->pixels) return chosen;
    for (unsigned i = 0; i < ui_active_font_count; ++i) {
        const UiFont& font = ui_active_fonts[i];
        if (pixels >= font.pixels) chosen = &font;
        else if (font.pixels - pixels < pixels - chosen->pixels) { chosen = &font; break; }
        else break;
    }
    return chosen;
}
static const UiGlyph* ui_font_glyph(const UiFont* font, uint8_t c) {
    return &font->glyphs[(c >= 32 && c <= 126 ? c : '?') - 32];
}
static int32_t ui_text_width(const UiFont* font, const char* text) {
    int32_t width = 0;
    if (text) for (; *text; ++text) width += ui_font_glyph(font, (uint8_t)*text)->advance;
    return width;
}
static uint32_t ui_font_blend(uint32_t background, uint32_t ink, uint8_t coverage) {
    uint32_t result = 0xff000000;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        uint32_t c = (((ink >> shift) & 255) * coverage +
                      ((background >> shift) & 255) * (255 - coverage) + 127) / 255;
        result |= c << shift;
    }
    return result;
}
// Clip is exclusive at right/bottom. The caller supplies the active compositor
// damage rectangle as well as any widget-specific text bounds.
static void ui_text_draw(uint32_t* buffer, uint32_t width, uint32_t height, uint32_t stride,
                         const UiFont* font, int32_t x, int32_t y, const char* text, uint32_t ink,
                         int32_t left, int32_t top, int32_t right, int32_t bottom) {
    if (!buffer || !font || !text || stride < width) return;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > (int32_t)width) right = width;
    if (bottom > (int32_t)height) bottom = height;
    if (left >= right || top >= bottom) return;
    for (; *text && x < right; ++text) {
        const UiGlyph* glyph = ui_font_glyph(font, (uint8_t)*text);
        int32_t gx = x + glyph->left, gy = y + font->ascent - glyph->top;
        for (unsigned row = 0; row < glyph->height; ++row) {
            int32_t py = gy + row;
            if (py < top || py >= bottom) continue;
            for (unsigned col = 0; col < glyph->width; ++col) {
                int32_t px = gx + col;
                if (px < left || px >= right) continue;
                uint8_t a = font->coverage[glyph->offset + row * glyph->width + col];
                if (a) {
                    uint32_t* pixel = buffer + (uint64_t)py * stride + px;
                    *pixel = ui_font_blend(*pixel, ink, a);
                }
            }
        }
        x += glyph->advance;
    }
}
#endif
