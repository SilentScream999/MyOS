#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <stdint.h>
#include "font8x8_basic.h"

// A generic rendering target. This could be a window's backbuffer,
// or a wrapper around the global framebuffer.
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch; // in bytes
    uint32_t* buffer;
} Canvas;

static inline void draw_rect(Canvas* c, int x, int y, int w, int h, uint32_t color) {
    if (!c || !c->buffer) return;
    
    // Bounds clipping
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)c->width) w = c->width - x;
    if (y + h > (int)c->height) h = c->height - y;
    if (w <= 0 || h <= 0) return;

    uint32_t pitch32 = c->pitch / 4;
    for (int row = y; row < y + h; row++) {
        uint32_t* row_base = c->buffer + row * pitch32;
        // In the future we can use REP STOSD or SSE here for performance
        for (int col = x; col < x + w; col++) {
            row_base[col] = color;
        }
    }
}

static inline void draw_border(Canvas* c, int x, int y, int w, int h, int thickness, uint32_t color) {
    // Top
    draw_rect(c, x, y, w, thickness, color);
    // Bottom
    draw_rect(c, x, y + h - thickness, w, thickness, color);
    // Left
    draw_rect(c, x, y, thickness, h, color);
    // Right
    draw_rect(c, x + w - thickness, y, thickness, h, color);
}

static inline void draw_char_on(Canvas* c, int x, int y, char ch, uint32_t color) {
    if (!c || !c->buffer) return;
    // Fast bounds reject
    if (x < 0 || y < 0 || x + 8 > (int)c->width || y + 8 > (int)c->height) return;
    
    const uint8_t* glyph = font8x8_basic[(int)ch];
    uint32_t pitch32 = c->pitch / 4;
    
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        uint32_t* row_base = c->buffer + (y + row) * pitch32 + x;
        for (int col = 0; col < 8; col++) {
            if (bits & (1 << col)) row_base[col] = color;
        }
    }
}

static inline void draw_string_on(Canvas* c, int x, int y, const char* str, uint32_t color) {
    int cur_x = x;
    while (*str) {
        draw_char_on(c, cur_x, y, *str, color);
        cur_x += 8;
        str++;
    }
}

static inline void blit_canvas(Canvas* dst, int dx, int dy, Canvas* src) {
    if (!dst || !src || !dst->buffer || !src->buffer) return;
    
    int src_x = 0;
    int src_y = 0;
    int copy_w = src->width;
    int copy_h = src->height;

    // Clip left/top
    if (dx < 0) { src_x = -dx; copy_w += dx; dx = 0; }
    if (dy < 0) { src_y = -dy; copy_h += dy; dy = 0; }

    // Clip right/bottom
    if (dx + copy_w > (int)dst->width)  copy_w = dst->width - dx;
    if (dy + copy_h > (int)dst->height) copy_h = dst->height - dy;

    if (copy_w <= 0 || copy_h <= 0) return;

    uint32_t dpitch = dst->pitch / 4;
    uint32_t spitch = src->pitch / 4;

    for (int y = 0; y < copy_h; y++) {
        uint32_t* dst_row = dst->buffer + (dy + y) * dpitch + dx;
        uint32_t* src_row = src->buffer + (src_y + y) * spitch + src_x;
        for (int x = 0; x < copy_w; x++) {
            uint32_t pixel = src_row[x];
            // Simple 1-bit alpha transparency: if highest byte is not 0, draw it.
            if ((pixel & 0xFF000000) != 0) {
                dst_row[x] = pixel;
            }
        }
    }
}

#endif // GRAPHICS_H

