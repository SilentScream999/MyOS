#ifndef TERMINAL_H
#define TERMINAL_H

#include "framebufferstuff.h"
#include <stdint.h>
#include <stddef.h>

// Runtime-configurable terminal window origin and bounds.
// Set by the WM before the shell starts (in kernel.cpp / wm_init).
extern uint32_t g_term_ox;
extern uint32_t g_term_oy;
extern uint32_t g_term_max_cols;
extern uint32_t g_term_max_rows;
extern uint32_t g_term_history_rows;
extern uint32_t g_term_total_rows;

static uint32_t term_x = 0;
static uint32_t term_y = 0;
static uint32_t term_color = 0xFFFFFFFF; // White

static bool term_escape = false;
static bool term_escape_bracket = false;
static char term_escape_buf[16];
static int term_escape_len = 0;

static void term_clear_screen() {
    wm_dirty_chrome();
    g_voffset = 0; // Reset circularity
    g_view_delta = 0;
    if (g_backbuffer) {
        memset(g_backbuffer, 0, g_term_buf_height * fb->pitch);
    }
    term_dirty_all();
    g_term_history_rows = 0;
    g_term_total_rows   = 0;
    term_x = 0;
    term_y = 0;
}

static void term_scroll() {
    uint32_t row_height = 10;
    if (g_term_buf_height <= row_height) return;

    g_voffset = (g_voffset + row_height) % g_term_buf_height;
    g_view_delta = 0;

    // Increment history up to the virtual buffer's capacity
    uint32_t max_history = g_term_buf_height / 10 - g_term_max_rows;
    g_term_total_rows++;
    if (g_term_history_rows < max_history) {
        g_term_history_rows++;
    }

    // Only mark the actual terminal window content rows as dirty.
    term_dirty_all();
    
    // Clear the new bottom row that just scrolled into view.
    // Important: we map the physical (g_term_max_rows - 1) row through the virtual mapping!
    // Since putp requires virtual mapped coordinates internally, we leave this as physical
    uint32_t last_row_y = (g_term_max_rows - 1) * 10 + g_term_oy;
    for (uint32_t y = last_row_y; y < last_row_y + 10; y++) {
        for (uint32_t x = g_term_ox; x < g_term_ox + g_term_max_cols * 8; x++) {
            putp(x, y, 0x00000000);
        }
    }
}

static void term_scroll_view(int delta) {
    if (delta == 0) return;
    // Clamp scrollback to the actual typed history, up to the buffer capacity.
    // We can scroll back 'g_term_history_rows' lines from the current view.
    int32_t max_scroll_back = (int32_t)(g_term_history_rows);
    if (max_scroll_back > (int32_t)(g_term_total_rows - g_term_max_rows))
        max_scroll_back = (int32_t)(g_term_total_rows - g_term_max_rows);

    g_view_delta += delta;
    if (g_view_delta < -max_scroll_back) g_view_delta = -max_scroll_back;
    if (g_view_delta > 0)                g_view_delta = 0;

    term_dirty_all();
}

// Erase the cursor from the backbuffer (call before drawing a character)
static void term_erase_cursor() {
    uint32_t cx = term_x * 8;
    uint32_t cy = term_y * 10;
    for (int r = 8; r < 10; r++) {
        for (int col = 0; col < 8; col++) {
            putp(cx + col, cy + r, 0x00000000);
        }
    }
    uint32_t dy1 = cy + 8;
    uint32_t dy2 = cy + 10;
    if (dy1 < g_dirty_min_y) g_dirty_min_y = dy1;
    if (dy2 > g_dirty_max_y) g_dirty_max_y = dy2;
}

static void term_putchar(char c) {
    uint32_t term_width  = g_term_max_cols;
    uint32_t term_height = g_term_max_rows;

    // Erase the cursor from backbuffer before drawing so it doesn't ghost
    term_erase_cursor();

    // Mark current line dirty
    uint32_t x1 = term_x * 8 + g_term_ox;
    uint32_t x2 = x1 + 8;
    uint32_t y1 = term_y * 10 + g_term_oy;
    uint32_t y2 = y1 + 10;
    term_dirty_rect(x1, y1, x2, y2);

    if (term_escape) {
        if (!term_escape_bracket) {
            if (c == '[') {
                term_escape_bracket = true;
                term_escape_len = 0;
            } else {
                term_escape = false;
                term_escape_bracket = false;
            }
            return;
        }
        
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            if (c == 'J' && term_escape_len > 0 && term_escape_buf[0] == '2') {
                term_clear_screen();
            } else if (c == 'H') {
                term_x = 0;
                term_y = 0;
            } else if (c == 'm') {
                if (term_escape_len == 0 || (term_escape_buf[0] == '0')) {
                    term_color = 0xFFFFFFFF;
                } else if (term_escape_len >= 2 && term_escape_buf[0] == '3' && term_escape_buf[1] == '2') {
                    term_color = 0xFF00FF00;
                }
            } else if (c == 'K') {
                for (uint32_t tx = term_x; tx < term_width; tx++) {
                    for (int r = 0; r < 10; r++) {
                        for (int col = 0; col < 8; col++) {
                            putp(tx * 8 + col, term_y * 10 + r, 0x00000000);
                        }
                    }
                }
            }
            term_escape = false;
            term_escape_bracket = false;
            return;
        }
        
        if (term_escape_len < 15) {
            term_escape_buf[term_escape_len++] = c;
        }
        return;
    }

    if (c == '\e') {
        term_escape = true;
        term_escape_bracket = false;
        return;
    }

    if (c == '\n') {
        term_x = 0;
        term_y++;
        // If we wrapped without scrolling, it's still history
        if (term_y < term_height && g_term_history_rows < term_y) {
             // g_term_history_rows = term_y; // Keep it simple for now
        }
    } else if (c == '\r') {
        term_x = 0;
    } else if (c == '\b') {
        if (term_x > 0) {
            term_x--;
            for (int r = 0; r < 10; r++) {
                for (int col = 0; col < 8; col++) {
                    putp(term_x * 8 + col, term_y * 10 + r, 0x00000000);
                }
            }
            term_dirty_rect(g_term_ox + term_x * 8, g_term_oy + term_y * 10, g_term_ox + (term_x + 1) * 8, g_term_oy + (term_y + 1) * 10);
        }
    } else {
        // Clear current cell relative area
        for (int r = 0; r < 10; r++) {
            for (int col = 0; col < 8; col++) {
                putp(term_x * 8 + col, term_y * 10 + r, 0x00000000);
            }
        }
        // Manual relative draw_char (8x8 glyph in 8x10 cell)
        const uint8_t* glyph = font8x8_basic[(uint8_t)c];
        for (int row = 0; row < 8; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (1 << col)) putp(term_x * 8 + col, term_y * 10 + row, term_color);
            }
        }
        term_dirty_rect(g_term_ox + term_x * 8, g_term_oy + term_y * 10, g_term_ox + (term_x + 1) * 8, g_term_oy + (term_y + 1) * 10);
        term_x++;
        if (term_x >= term_width) {
            term_x = 0;
            term_y++;
        }
    }

    if (term_y >= term_height) {
        term_scroll();
        term_y--;
    }
}

// Draw the cursor into the backbuffer so term_flip() includes it naturally.
// This eliminates flicker caused by VRAM-direct writes being overwritten by flip.
static void term_render_cursor() {
    uint32_t cx = term_x * 8;
    uint32_t cy = term_y * 10;
    for (int r = 8; r < 10; r++) {
        for (int col = 0; col < 8; col++) {
            putp(cx + col, cy + r, term_color);
        }
    }
    // Mark cursor rows dirty (absolute screen space)
    uint32_t dy1 = cy + g_term_oy + 8;
    uint32_t dy2 = cy + g_term_oy + 10;
    if (dy1 < g_dirty_min_y) g_dirty_min_y = dy1;
    if (dy2 > g_dirty_max_y) g_dirty_max_y = dy2;
}

static void term_write(const char* data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        term_putchar(data[i]);
    }
    term_render_cursor();
    g_needs_refresh = true;
}

static void term_write_string(const char* str) {
    while (*str) {
        term_putchar(*str++);
    }
    term_render_cursor();
    g_needs_refresh = true;
}

#endif // TERMINAL_H
