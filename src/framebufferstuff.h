#ifndef frames_h
#define frames_h

#include <stdint.h>
#include "font8x8_basic.h"
#include "string.h"
#include "klog.h"

extern "C" {
    #include "../limine.h"
}

static struct limine_framebuffer *fb;
extern uint8_t* g_backbuffer;
extern uint8_t* g_master_backbuffer;
extern uint32_t g_voffset;
extern volatile uint32_t g_dirty_min_y;
extern volatile uint32_t g_dirty_max_y;
extern volatile uint32_t g_dirty_min_x;
extern volatile uint32_t g_dirty_max_x;
extern int32_t  g_view_delta;   // rows scrolled (0=live, negative=up)
extern uint32_t g_term_buf_height; // pixels in virtual backbuffer (e.g. 20000)
extern uint32_t g_term_ox;        // terminal window X offset
extern uint32_t g_term_oy;        // terminal window Y offset
extern uint32_t g_term_max_cols;  // terminal character width
extern uint32_t g_term_max_rows;  // terminal row height
extern volatile bool g_needs_refresh;
extern volatile bool wm_chrome_dirty;
extern volatile bool g_dragging_test;
extern volatile bool g_dragging_term;
static inline void wm_dirty_chrome() { wm_chrome_dirty = true; g_needs_refresh = true; }


static inline void term_dirty_rect(uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2) {
    if (x1 < g_dirty_min_x) g_dirty_min_x = x1;
    if (x2 > g_dirty_max_x) g_dirty_max_x = x2;
    if (y1 < g_dirty_min_y) g_dirty_min_y = y1;
    if (y2 > g_dirty_max_y) g_dirty_max_y = y2;
}

static inline void term_dirty_all() {
    if (!fb) return;
    uint32_t x1 = g_term_ox;
    uint32_t y1 = (g_term_oy > 30) ? g_term_oy - 30 : 0;
    uint32_t x2 = g_term_ox + g_term_max_cols * 8 + 10;
    uint32_t y2 = g_term_oy + g_term_max_rows * 10 + 10;
    term_dirty_rect(x1, y1, x2, y2);
    g_needs_refresh = true;
}

static inline void term_dirty_reset() {
    // Logic moved to kernel.cpp for 2D safety
}

// Post-flip hook: called after every VRAM update.
// Used by the WM to paint title bar chrome without touching the backbuffer.
typedef void (*post_flip_hook_fn)();
extern post_flip_hook_fn g_post_flip_hook;

// Pre-flip hook: called before every VRAM update.
// Used by the WM to erase transient overlays (like the mouse pointer).
typedef void (*pre_flip_hook_fn)();
extern pre_flip_hook_fn g_pre_flip_hook;

static inline void term_flip() {
    if (!g_backbuffer || !fb) return;
    if (g_dirty_min_y > g_dirty_max_y) return;

    uint32_t min = g_dirty_min_y;
    uint32_t max = g_dirty_max_y;
    if (max >= fb->height) max = fb->height - 1;

    // g_view_delta is in rows (negative = scrolled up).
    // Add it to the backbuffer lookup so the display shows an earlier part of the ring.
    int32_t delta_px = g_view_delta * 10;   // convert rows→pixels

    if (g_pre_flip_hook) g_pre_flip_hook();

    uint32_t term_bottom = g_term_oy + g_term_max_rows * 10;
    for (uint32_t y = min; y <= max; y++) {
        // Essential: only blit from backbuffer if the line is inside the terminal window
        if (y < g_term_oy || y >= term_bottom) continue;

        // Where this screen row maps to in the circular backbuffer
        int32_t src = (int32_t)(y + g_voffset) + delta_px;
        // Keep positive and in range (using the virtual buffer height)
        src = ((src % (int32_t)g_term_buf_height) + (int32_t)g_term_buf_height) % (int32_t)g_term_buf_height;
        
        uint32_t x_off = g_term_ox;
        uint32_t width_px = g_term_max_cols * 8;
        
        memcpy_vram_sse((uint8_t*)fb->address + y * fb->pitch + x_off * 4,
                        g_backbuffer + (uint32_t)src * fb->pitch + x_off * 4,
                        width_px * 4);
    }
    term_dirty_reset();
    if (g_post_flip_hook) g_post_flip_hook();
}

// Writes a pixel to the circular terminal backbuffer at window-relative (rx, ry)
static inline void putp(uint32_t rx, uint32_t ry, uint32_t argb) {
    if (!g_backbuffer) {
        volatile uint32_t *vram = (volatile uint32_t *)fb->address;
        vram[ry * (fb->pitch / 4) + rx] = argb;
        return;
    }
    // Mapping through circular vertical offset
    uint32_t real_y = (ry + g_voffset) % g_term_buf_height;
	uint32_t *base = (uint32_t *)g_backbuffer;
	base[real_y * (fb->pitch / 4) + rx] = argb;
}

static inline void fill_screen_fast(uint32_t argb) {
    if (!g_backbuffer) {
        volatile uint8_t* base = (uint8_t*)fb->address;
        const uint32_t w = fb->width;
        for (uint32_t y = 0; y < fb->height; ++y) {
            volatile uint32_t* row = (uint32_t*)(base + y * fb->pitch);
            for (uint32_t x = 0; x < w; ++x) row[x] = argb;
        }
        return;
    }
    if (argb == 0) {
        memset(g_backbuffer, 0, g_term_buf_height * fb->pitch);
    } else {
        uint32_t* base = (uint32_t*)g_backbuffer;
        uint64_t count = (g_term_buf_height * fb->pitch) / 4;
        for (uint64_t i = 0; i < count; i++) base[i] = argb;
    }
    term_dirty_all();
}

static void draw_char(int x, int y, char c, uint32_t color) {
    if (x < 0 || y < 0 || x + 8 > fb->width || y + 8 > fb->height) return;
    if (!g_backbuffer) return;
    
    const uint8_t* glyph = font8x8_basic[(int)c];
    uint32_t* base = (uint32_t*)g_backbuffer;
    uint32_t pitch32 = fb->pitch / 4;
    
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        uint32_t real_y = (y + row + g_voffset) % g_term_buf_height;
        uint32_t* row_base = base + real_y * pitch32 + x;
        for (int col = 0; col < 8; col++) {
            if (bits & (1 << col)) row_base[col] = color;
        }
    }

    term_dirty_rect(x, y, x + 8, y + 8);
}

static int current_print_line = 0;
static int max_print_lines = 0;

static void print(char* buffer) {
    klog_print(buffer);
    klog_putc('\n');

    if (g_klog_bypass_framebuffer) return;

    if (max_print_lines == 0) {
        max_print_lines = fb->height / 10;
    }
    
    if (current_print_line >= max_print_lines) {
        current_print_line = 0;
        fill_screen_fast(0x00000000);
    }
    
    uint64_t i = 0;
    while (true) {
        char c = buffer[i];
        if (c == '\0') break;
        draw_char(i*8, current_print_line*10, c, 0xFFFFFFFF);
        i++;
    }
    current_print_line ++;
    g_needs_refresh = true;
}

#endif