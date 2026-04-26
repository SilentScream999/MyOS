#ifndef TERMINAL_H
#define TERMINAL_H

#include "framebufferstuff.h"
#include <stdint.h>
#include <stddef.h>

// Runtime-configurable terminal window origin and bounds.
// Set by the WM before the shell starts (in kernel.cpp / wm_init).
extern int32_t g_term_ox;
extern int32_t g_term_oy;
extern uint32_t g_term_max_cols;
extern uint32_t g_term_max_rows;
static uint32_t g_term_scrolled_rows = 0; // lines that have scrolled off the top of the screen
static int32_t  g_term_view_offset = 0;   // 0 = live, -1 = one line back

static uint32_t term_x = 0;
static uint32_t term_y = 0;
static uint32_t term_color = 0xFFFFFFFF; // White

static bool term_escape = false;
static bool term_escape_bracket = false;
static char term_escape_buf[16];
static int term_escape_len = 0;

// ── Matrix-First Terminal Storage ─────────────────────────────────────────────
#define TERM_MATRIX_ROWS 500
#define TERM_MATRIX_COLS 160
struct TermCell {
    uint8_t c;
    uint32_t color;
};
static TermCell g_term_matrix[TERM_MATRIX_ROWS][TERM_MATRIX_COLS];
static uint32_t g_voffset = 0; // Dummy for wm.h compatibility

// ── Asynchronous Ring Buffer ──────────────────────────────────────────────────
#define TERM_RING_SIZE  32768
static char     g_term_ring[TERM_RING_SIZE];
static uint32_t g_term_ring_head = 0; // write index
static uint32_t g_term_ring_tail = 0; // read index
static volatile bool g_term_ring_lock = false;

static void term_enqueue_text(const char* data, size_t size) {
    while (__sync_lock_test_and_set(&g_term_ring_lock, 1));
    for (size_t i = 0; i < size; i++) {
        uint32_t next = (g_term_ring_head + 1) % TERM_RING_SIZE;
        if (next == g_term_ring_tail) break; // full
        g_term_ring[g_term_ring_head] = data[i];
        g_term_ring_head = next;
    }
    __sync_lock_release(&g_term_ring_lock);
}

static void term_clear_screen() {
    wm_dirty_chrome();
    g_term_view_offset = 0;
    if (g_backbuffer) {
        memset_32(g_backbuffer, 0, (g_term_max_rows * 10) * (fb->pitch / 4));
    }
    // Clear matrix
    for (int y = 0; y < TERM_MATRIX_ROWS; y++) {
        for (int x = 0; x < TERM_MATRIX_COLS; x++) {
            g_term_matrix[y][x].c = 0;
            g_term_matrix[y][x].color = 0xFFFFFFFF;
        }
    }
    term_dirty_all();
    g_term_scrolled_rows = 0;
    term_x = 0;
    term_y = 0;
}

// Redraw the entire pixel backbuffer from the matrix based on view_offset
static void term_sync_backbuffer() {
    if (!g_backbuffer) return;
    uint32_t pitch32 = fb->pitch / 4;
    uint32_t* base = (uint32_t*)g_backbuffer;

    // Determine viewport start in matrix
    int32_t start_row = (int32_t)g_term_scrolled_rows + g_term_view_offset;
    if (start_row < 0) start_row = 0;

    for (uint32_t ty = 0; ty < g_term_max_rows; ty++) {
        uint32_t my = (uint32_t)(start_row + ty);
        uint32_t py = ty * 10;
        
        // Clear line background (Solid Dark Navy)
        for (int r = 0; r < 10; r++) {
            memset_32(base + (py + r) * pitch32, 0xFF0A0A14, g_term_max_cols * 8);
        }

        if (my >= (g_term_scrolled_rows + g_term_max_rows)) continue;

        for (uint32_t tx = 0; tx < g_term_max_cols; tx++) {
            char c = (char)g_term_matrix[my][tx].c;
            if (c == 0 || c == ' ') continue;
            uint32_t clr = g_term_matrix[my][tx].color;
            const uint8_t* glyph = font8x8_basic[(uint8_t)c];
            for (int r = 0; r < 8; r++) {
                uint8_t bits = glyph[r];
                uint32_t* row_base = base + (py + r) * pitch32 + (tx * 8);
                for (int col = 0; col < 8; col++) {
                    if (bits & (1 << col)) row_base[col] = clr;
                }
            }
        }
    }
    term_dirty_all();
}

static void term_scroll() {
    if (g_term_scrolled_rows + g_term_max_rows < TERM_MATRIX_ROWS) {
        g_term_scrolled_rows++;
    } else {
        // Shift matrix up
        memmove(g_term_matrix, &g_term_matrix[1], (TERM_MATRIX_ROWS - 1) * sizeof(g_term_matrix[0]));
        // g_term_scrolled_rows stays same (it's at max)
        // Clear bottom row
        for (int x = 0; x < TERM_MATRIX_COLS; x++) {
            g_term_matrix[TERM_MATRIX_ROWS - 1][x].c = 0;
            g_term_matrix[TERM_MATRIX_ROWS - 1][x].color = 0xFFFFFFFF;
        }
    }
    
    // Pixel scroll (Delta)
    if (g_backbuffer && g_term_view_offset == 0) {
        uint32_t pitch32 = fb->pitch / 4;
        uint32_t window_height_px = (g_term_max_rows - 1) * 10;
        uint32_t ww_px = g_term_max_cols * 8;
        
        // Shift ONLY the active terminal content (avoids junk at the pitch-edge)
        for (uint32_t y = 0; y < window_height_px; y++) {
            uint32_t* src = (uint32_t*)g_backbuffer + (y + 10) * pitch32;
            uint32_t* dst = (uint32_t*)g_backbuffer + y * pitch32;
            memcpy(dst, src, ww_px * 4);
        }

        // Clear ONLY the bottom row with the solid color
        for (uint32_t y = window_height_px; y < window_height_px + 10; y++) {
            memset_32((uint32_t*)g_backbuffer + y * pitch32, 0xFF0A0A14, ww_px);
        }
    }
}

static void term_scroll_view(int delta) {
    int32_t old = g_term_view_offset;
    g_term_view_offset += delta;
    int32_t max_back = (int32_t)g_term_scrolled_rows;
    
    if (g_term_view_offset < -max_back) g_term_view_offset = -max_back;
    if (g_term_view_offset > 0) g_term_view_offset = 0;

    if (g_term_view_offset != old) {
        term_sync_backbuffer();
        g_needs_refresh = true;
    }
}

// Erase the cursor from the backbuffer (call before drawing a character)
static void term_erase_cursor() {
    uint32_t cx = term_x * 8;
    uint32_t cy = term_y * 10;
    uint32_t* base = (uint32_t*)g_backbuffer;
    uint32_t pitch32 = fb->pitch / 4;
    for (int r = 8; r < 10; r++) {
        uint32_t* row_base = base + (cy + r) * pitch32 + cx;
        for (int col = 0; col < 8; col++) row_base[col] = 0;
    }
    term_dirty_rect(cx + g_term_ox, cy + g_term_oy + 8, cx + g_term_ox + 8, cy + g_term_oy + 10);
}

static void term_putchar(char c) {
    uint32_t term_width  = g_term_max_cols;
    uint32_t term_height = g_term_max_rows;

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
        // Update matrix
        uint32_t my = g_term_scrolled_rows + term_y;
        if (my < TERM_MATRIX_ROWS && term_x < g_term_max_cols) {
            g_term_matrix[my][term_x].c = (uint8_t)c;
            g_term_matrix[my][term_x].color = term_color;
        }

        if (g_term_view_offset == 0) {
            uint32_t cx = term_x * 8;
            uint32_t cy = term_y * 10;
            uint32_t* base = (uint32_t*)g_backbuffer;
            uint32_t pitch32 = fb->pitch / 4;
            const uint8_t* glyph = font8x8_basic[(uint8_t)c];
            
            // Draw character with solid background to prevent ghosting
            for (int row = 0; row < 10; row++) {
                uint8_t bits = (row < 8) ? glyph[row] : 0;
                uint32_t* row_ptr = base + (cy + row) * pitch32 + cx;
                for (int col = 0; col < 8; col++) {
                    row_ptr[col] = (bits & (1 << col)) ? term_color : 0xFF0A0A14;
                }
            }
            term_dirty_rect(g_term_ox + cx, g_term_oy + cy, g_term_ox + cx + 8, g_term_oy + cy + 10);
        }
        term_x++;
        if (term_x >= term_width) {
            term_x = 0; term_y++;
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
    uint32_t* base = (uint32_t*)g_backbuffer;
    uint32_t pitch32 = fb->pitch / 4;

    for (int r = 8; r < 10; r++) {
        uint32_t* row_base = base + (cy + r) * pitch32 + cx;
        for (int col = 0; col < 8; col++) row_base[col] = term_color;
    }
    term_dirty_rect(cx + g_term_ox, cy + g_term_oy + 8, cx + g_term_ox + 8, cy + g_term_oy + 10);
}

static void term_write(const char* data, size_t size) {
    term_enqueue_text(data, size);
    g_needs_refresh = true;
}

static void term_write_string(char* str) {
    size_t len = 0;
    while (str[len]) len++;
    term_enqueue_text(str, len);
    g_needs_refresh = true;
}

static void term_process_ring_buffer() {
    if (g_term_ring_head == g_term_ring_tail) return;

    term_erase_cursor();
    
    // Budget: process at most 4096 characters per frame (~250 KB/s)
    // This prevents the compositor from stalling on huge cat dumps.
    uint32_t budget = 4096;
    bool moved = false;
    
    while (g_term_ring_tail != g_term_ring_head && budget-- > 0) {
        term_putchar(g_term_ring[g_term_ring_tail]);
        g_term_ring_tail = (g_term_ring_tail + 1) % TERM_RING_SIZE;
        moved = true;
    }

    if (moved) {
        term_render_cursor();
        term_dirty_all(); // efficient batch update
        g_needs_refresh = true;
    }
}

#endif // TERMINAL_H
