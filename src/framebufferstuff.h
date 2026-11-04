#ifndef frames_h
#define frames_h

#include <stdint.h>
#include "font8x8_basic.h"
#include "string.h"
#include "klog.h"
#include "damage.h"

extern "C" {
    #include "../limine.h"
}

extern struct limine_framebuffer *fb;
extern uint8_t* g_backbuffer;
extern uint8_t* g_master_backbuffer;
extern volatile uint32_t g_dirty_min_y;
extern volatile uint32_t g_dirty_max_y;
extern volatile uint32_t g_dirty_min_x;
extern volatile uint32_t g_dirty_max_x;
extern int32_t g_term_ox;        // terminal window X offset
extern int32_t g_term_oy;        // terminal window Y offset
extern uint32_t g_term_max_cols;  // terminal character width
extern uint32_t g_term_max_rows;  // terminal row height
extern volatile bool g_needs_refresh;
extern DamageList g_damage;     // separate dirty rects (see damage.h); the g_dirty_* box still exists for legacy writers

// Mark a screen region dirty (min/max convention, same as the compositor). Interrupt-safe.
static inline void wm_dirty_add(uint32_t min_x, uint32_t max_x, uint32_t min_y, uint32_t max_y) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    if (fb) damage_add_clipped(&g_damage, fb->width, fb->height, min_x, max_x, min_y, max_y);
    if (flags & (1u << 9)) __asm__ volatile("sti" ::: "memory");
}
static inline void wm_dirty_chrome() { g_needs_refresh = true; }


static inline void term_dirty_rect(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    if (!fb) return;
    
    // Clamp to screen boundaries before updating unsigned dirty coords
    uint32_t ux1 = (x1 < 0) ? 0 : (uint32_t)x1;
    uint32_t uy1 = (y1 < 0) ? 0 : (uint32_t)y1;
    uint32_t ux2 = (x2 < 0) ? 0 : (uint32_t)x2;
    uint32_t uy2 = (y2 < 0) ? 0 : (uint32_t)y2;
    
    if (ux1 > fb->width) ux1 = fb->width;
    if (ux2 > fb->width) ux2 = fb->width;
    if (uy1 > fb->height) uy1 = fb->height;
    if (uy2 > fb->height) uy2 = fb->height;

    wm_dirty_add(ux1, ux2, uy1, uy2);   // was: union into the single global box
}

static inline void term_dirty_all() {
    if (!fb) return;
    int32_t x1 = g_term_ox;
    int32_t y1 = g_term_oy - 30;
    int32_t x2 = g_term_ox + (int32_t)g_term_max_cols * 8 + 10;
    int32_t y2 = g_term_oy + (int32_t)g_term_max_rows * 10 + 10;
    
    // Clamp to screen for the dirty rect logic (which uses uint32 internally)
    uint32_t ux1 = (x1 < 0) ? 0 : (uint32_t)x1;
    uint32_t uy1 = (y1 < 0) ? 0 : (uint32_t)y1;
    uint32_t ux2 = (x2 < 0) ? 0 : (uint32_t)x2;
    uint32_t uy2 = (y2 < 0) ? 0 : (uint32_t)y2;
    
    if (ux1 > fb->width) ux1 = fb->width;
    if (ux2 > fb->width) ux2 = fb->width;
    if (uy1 > fb->height) uy1 = fb->height;
    if (uy2 > fb->height) uy2 = fb->height;
    
    term_dirty_rect(ux1, uy1, ux2, uy2);
    g_needs_refresh = true;
}

static inline void term_dirty_reset() {
    // Logic moved to kernel.cpp for 2D safety
}

// Post-flip hook: called after every VRAM update.
// Used by the WM to paint title bar chrome without touching the backbuffer.
typedef void (*post_flip_hook_fn)();
extern post_flip_hook_fn g_post_flip_hook;

// GPU flip hook: if set, wm_compose_dirty() asks the GPU to copy the dirty rect from
// g_master_backbuffer to the screen. Returns true on success; false => CPU fallback.
typedef bool (*gpu_flip_hook_fn)(uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2);
extern gpu_flip_hook_fn g_gpu_flip_hook;
// Short GPU status shown in the HUD (benchmark result / which copy path is active).
extern char g_gpu_status[64];

// Pre-flip hook: called before every VRAM update.
// Used by the WM to erase transient overlays (like the mouse pointer).
typedef void (*pre_flip_hook_fn)();
extern pre_flip_hook_fn g_pre_flip_hook;

static inline void term_flip() {
    // REDUNDANT: term_flip used to blit directly to VRAM, causing major CPU lag.
    // The compositor (wm.h) now handles blitting the terminal from g_backbuffer
    // into the unified pipeline at a steady 60Hz. Disabling this saves ~50% bus traffic.
    term_dirty_reset();
    g_needs_refresh = true;
}

static inline void putp(uint32_t rx, uint32_t ry, uint32_t argb) {
    if (!g_backbuffer) {
        volatile uint32_t *vram = (volatile uint32_t *)fb->address;
        vram[ry * (fb->pitch / 4) + rx] = argb;
        return;
    }
    uint32_t *base = (uint32_t *)g_backbuffer;
    uint32_t pitch32 = g_term_max_cols ? (g_term_max_cols * 8) : (fb ? fb->pitch / 4 : 800);
    base[ry * pitch32 + rx] = argb;
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
    uint32_t pitch32 = g_term_max_cols ? (g_term_max_cols * 8) : (fb ? fb->pitch / 4 : 800);
    uint64_t count = (uint64_t)g_term_max_rows * 10 * pitch32;
    uint32_t clr = (argb == 0) ? 0xFF0A0A14 : argb;
    uint32_t* base = (uint32_t*)g_backbuffer;
    for (uint64_t i = 0; i < count; i++) base[i] = clr;
    term_dirty_all();
}

static void draw_char(int x, int y, char c, uint32_t color) {
    if (x < 0 || y < 0) return;
    if (!g_backbuffer) return;
    
    uint32_t pitch32 = g_term_max_cols ? (g_term_max_cols * 8) : (fb ? fb->pitch / 4 : 800);
    uint32_t term_w_px = g_term_max_cols * 8;
    uint32_t term_h_px = g_term_max_rows * 10;
    if ((uint32_t)(x + 8) > term_w_px || (uint32_t)(y + 8) > term_h_px) return;

    const uint8_t* glyph = font8x8_basic[(int)c];
    uint32_t* base = (uint32_t*)g_backbuffer;
    
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        uint32_t* row_base = base + (y + row) * pitch32 + x;
        for (int col = 0; col < 8; col++) {
            if (bits & (1 << col)) row_base[col] = color;
        }
    }

    term_dirty_rect(g_term_ox + x, g_term_oy + y, g_term_ox + x + 8, g_term_oy + y + 8);
}

extern void wm_log_append(const char* s);

static int current_print_line = 0;
static int max_print_lines = 0;

static void print(char* buffer) {
    klog_print(buffer);
    klog_putc('\n');

    wm_log_append(buffer);

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