#ifndef WM_H
#define WM_H

#include <stdint.h>
#include <stddef.h>
#include "graphics.h"
#include "framebufferstuff.h"
#include "heap.h"
#include "mouse.h"
#include "string.h"

#include "font8x8_basic.h"

// ── RDTSC Helper ─────────────────────────────────────────────────────────────
static inline uint64_t rdtsc_val() {
    uint64_t val;
    __asm__ volatile("rdtsc; shl $32, %%rdx; or %%rdx, %%rax" : "=a"(val) :: "rdx");
    return val;
}

// Kernel globals defined in kernel.cpp / scheduler.h
extern uint64_t tsc_hz;
extern volatile uint64_t g_idle_tsc_accum;
extern uint64_t g_last_frame_ticks;

static uint64_t g_time_bg_ticks = 0;
static uint64_t g_time_win_ticks = 0;
static uint64_t g_time_rain_ticks = 0;
static uint64_t g_time_lock_ticks = 0;
static uint64_t g_time_vram_ticks = 0;

static uint32_t g_title_h = 28;
static uint32_t g_border  = 2;
static uint32_t g_pad     = 2;
#define CURSOR_W    12
#define CURSOR_H    19

#define WIN_TERM 0
#define WIN_TEST 1
#define WIN_RAIN 2
#define WIN_LOG  3

static int  g_win_order[4]   = {WIN_TERM, WIN_TEST, WIN_RAIN, WIN_LOG};
static bool g_win_visible[4] = {true, true, true, true};

static int32_t  test_x = 450, test_y = 120, test_w = 800, test_h = 600;
static int32_t  rain_x = 150, rain_y = 420, rain_w = 800, rain_h = 600;
static uint32_t rain_scroll = 0;
static uint32_t g_rainbow_lut[360];
static uint32_t wm_win_x, wm_win_y, wm_win_w, wm_win_h;
static uint32_t g_taskbar_h = 40; // Default, will be recalculated in wm_init
static uint32_t g_sb_w = 92;
static uint32_t g_wbtn_w = 124;

// ── WIN_LOG: independent scrollable text window ──────────────────────────────
#define LOG_WIN_LINES 150
#define LOG_WIN_COLS  100
#define LOG_WIN_ROWS_VISIBLE 60
static uint32_t g_log_win_cols = 100;
static uint32_t g_log_rows_visible = 60;
static uint32_t g_log_stride = 800;
static uint32_t g_font_scale_100 = 100;
#define LOG_CHAR_W    8
#define LOG_CHAR_H    10
static int32_t  log_win_x = 20, log_win_y = 80;
static int32_t  log_win_scroll = 0; // rows scrolled up (positive = older content)
static char     g_log_buf[LOG_WIN_LINES][LOG_WIN_COLS + 1];
static uint32_t g_log_line_count = 0;
static uint32_t* g_log_win_backbuffer = nullptr;
static int32_t  log_last_rendered_scroll = -1;

static void wm_log_render_line_to_buffer(int32_t line_idx, int32_t buffer_row) {
    if (!g_log_win_backbuffer) return;
    uint32_t stride = g_log_stride;
    const char* line = g_log_buf[line_idx];
    uint32_t py = buffer_row * LOG_CHAR_H;
    uint32_t row_color = (line_idx % 2 == 0) ? 0xFFCCDDFF : 0xFFAABBDD;

    // Clear background for the entire stride to avoid artifacts
    for (int r = 0; r < LOG_CHAR_H; r++) {
        memset_32(g_log_win_backbuffer + (py + r) * stride, 0xFF0a0a14, stride);
    }

    for (int c = 0; line[c] && c < (int32_t)g_log_win_cols; c++) {
        uint32_t px = c * LOG_CHAR_W;
        uint8_t* glyph = (uint8_t*)font8x8_basic[(uint8_t)line[c]];
        for (int r = 0; r < 8; r++) {
            uint32_t* buf_row = g_log_win_backbuffer + (py + r) * stride + px;
            for (int b = 0; b < 8; b++) {
                if (glyph[r] & (1 << b)) buf_row[b] = row_color;
            }
        }
    }
}

static void wm_log_render_to_buffer() {
    if (!g_log_win_backbuffer) return;
    if (log_win_scroll == log_last_rendered_scroll) return;

    uint32_t stride = g_log_stride;
    uint32_t bh = g_log_rows_visible * LOG_CHAR_H;

    // Delta-scroll optimization: if we moved by exactly 1 line, memmove and draw 1 line
    int32_t delta = log_win_scroll - log_last_rendered_scroll;
    if (log_last_rendered_scroll != -1 && delta == 1) {
        // Scrolled DOWN (content moves UP)
        memmove(g_log_win_backbuffer, g_log_win_backbuffer + stride * LOG_CHAR_H, stride * (g_log_rows_visible - 1) * LOG_CHAR_H * 4);
        wm_log_render_line_to_buffer(log_win_scroll + (int32_t)g_log_rows_visible - 1, (int32_t)g_log_rows_visible - 1);
    } else if (log_last_rendered_scroll != -1 && delta == -1) {
        // Scrolled UP (content moves DOWN)
        memmove(g_log_win_backbuffer + stride * LOG_CHAR_H, g_log_win_backbuffer, stride * (g_log_rows_visible - 1) * LOG_CHAR_H * 4);
        wm_log_render_line_to_buffer(log_win_scroll, 0);
    } else {
        // Full refresh
        memset_32(g_log_win_backbuffer, 0xFF0a0a14, stride * bh);
        for (int32_t r = 0; r < (int32_t)g_log_rows_visible; r++) {
            int32_t line_idx = log_win_scroll + r;
            if (line_idx >= 0 && line_idx < (int32_t)g_log_line_count)
                wm_log_render_line_to_buffer(line_idx, r);
        }
    }
    log_last_rendered_scroll = log_win_scroll;
}

static void wm_log_init() {
    for (int i = 0; i < LOG_WIN_LINES; i++) {
        // Fill with sample text so we have content to scroll
        const char* prefix = "Log line ";
        int j = 0;
        while (prefix[j] && j < LOG_WIN_COLS) { g_log_buf[i][j] = prefix[j]; j++; }
        // Append line number
        uint32_t n = i + 1;
        char tmp[8]; int tl = 0;
        if (n == 0) { tmp[tl++] = '0'; }
        else { uint32_t x = n; while (x) { tmp[tl++] = '0' + x % 10; x /= 10; } }
        for (int k = tl - 1; k >= 0 && j < LOG_WIN_COLS; k--) g_log_buf[i][j++] = tmp[k];
        // Pad rest with info chars to make lines visually substantial
        const char* info = " | compositor test | abcdefghijklmnop";
        int il = 0;
        while (info[il] && j < LOG_WIN_COLS) { g_log_buf[i][j] = info[il]; j++; il++; }
        g_log_buf[i][j] = '\0';
    }
    g_log_line_count = LOG_WIN_LINES;
    
    g_log_stride = g_log_win_cols * LOG_CHAR_W;
    uint32_t total_px = g_log_stride * (g_log_rows_visible * LOG_CHAR_H);
    g_log_win_backbuffer = (uint32_t*)kmalloc(total_px * 4);
    wm_log_render_to_buffer();
}

static void wm_log_scroll(int delta) {
    log_win_scroll += delta;
    if (log_win_scroll < 0) log_win_scroll = 0;
    int32_t max_scroll = (int32_t)g_log_line_count - (int32_t)g_log_rows_visible;
    if (max_scroll < 0) max_scroll = 0;
    if (log_win_scroll > max_scroll) log_win_scroll = max_scroll;
    
    // Prerender to backbuffer
    wm_log_render_to_buffer();

    // Mark log window area dirty
    if (fb) {
        uint32_t lx1 = (uint32_t)(log_win_x > 0 ? log_win_x - 4 : 0);
        uint32_t lx2 = (uint32_t)(log_win_x + LOG_WIN_COLS * LOG_CHAR_W + 8);
        uint32_t ly1 = (uint32_t)(log_win_y > 32 ? log_win_y - 32 : 0);
        uint32_t ly2 = (uint32_t)(log_win_y + LOG_WIN_ROWS_VISIBLE * LOG_CHAR_H + 8);
        if (lx1 < g_dirty_min_x) g_dirty_min_x = lx1;
        if (lx2 > g_dirty_max_x) g_dirty_max_x = lx2;
        if (ly1 < g_dirty_min_y) g_dirty_min_y = ly1;
        if (ly2 > g_dirty_max_y) g_dirty_max_y = ly2;
        g_needs_refresh = true;
    }
}

extern uint32_t g_term_ox, g_term_oy, g_term_max_cols, g_term_max_rows;

struct WinRect {
    int32_t x1, y1, x2, y2;
    bool visible;
};

static inline void get_win_rect(int idx, WinRect* r) {
    r->visible = g_win_visible[idx];
    if (idx == WIN_TERM) {
        uint32_t wx = g_term_ox, wy = g_term_oy;
        uint32_t ww = g_term_max_cols * 8, wh = g_term_max_rows * 10;
        r->x1 = (wx > (uint32_t)(g_border + g_pad)) ? wx - g_border - g_pad : 0;
        r->x2 = wx + ww + g_border + g_pad;
        r->y1 = (wy > (uint32_t)(g_border + g_title_h + g_pad)) ? (int32_t)(wy - g_border - g_title_h - g_pad) : 0;
        r->y2 = (int32_t)(wy + wh + g_border + g_pad);
    } else if (idx == WIN_TEST) {
        r->x1 = test_x; r->y1 = test_y - (int32_t)g_title_h;
        r->x2 = test_x + (int32_t)test_w; r->y2 = test_y + (int32_t)test_h;
    } else if (idx == WIN_RAIN) {
        r->x1 = rain_x; r->y1 = rain_y - (int32_t)g_title_h;
        r->x2 = rain_x + (int32_t)rain_w; r->y2 = rain_y + (int32_t)rain_h;
    } else if (idx == WIN_LOG) {
        int32_t lw = (int32_t)g_log_win_cols * LOG_CHAR_W;
        int32_t lh = (int32_t)g_log_rows_visible * LOG_CHAR_H; 
        r->x1 = log_win_x - (int32_t)(g_border + g_pad);
        r->y1 = log_win_y - (int32_t)(g_title_h + g_border + g_pad);
        r->x2 = log_win_x + lw + (int32_t)(g_border + g_pad);
        r->y2 = log_win_y + lh + (int32_t)(g_border + g_pad);
    }
}

static inline void wm_draw_char(uint32_t x, uint32_t y, char c, uint32_t color, uint32_t scale_100 = 100) {
    if (!g_master_backbuffer || (uint32_t)c > 127) return;
    uint8_t* glyph = (uint8_t*)font8x8_basic[(uint8_t)c];
    
    if (scale_100 == 150) {
        // 1.5x custom pattern (8px -> 12px)
        for (int r = 0; r < 8; r++) {
            int dr = (r / 2) * 3 + (r % 2 == 1 ? 2 : 0);
            int rh = (r % 2 == 0) ? 2 : 1;
            for (int rsy = 0; rsy < rh; rsy++) {
                uint32_t dy = y + (uint32_t)(dr + rsy);
                if (dy >= fb->height) continue;
                uint32_t* row = (uint32_t*)(g_master_backbuffer + (uint64_t)dy * fb->pitch);
                for (int b = 0; b < 8; b++) {
                    if (glyph[r] & (1 << b)) {
                        int db = (b / 2) * 3 + (b % 2 == 1 ? 2 : 0);
                        int bw = (b % 2 == 0) ? 2 : 1;
                        for (int bsx = 0; bsx < bw; bsx++) {
                            uint32_t dx = x + (uint32_t)(db + bsx);
                            if (dx < fb->width) row[dx] = color;
                        }
                    }
                }
            }
        }
    } else {
        // Generic integer scaling
        uint32_t s = scale_100 / 100;
        if (s < 1) s = 1;
        for (int r = 0; r < 8; r++) {
            for (uint32_t sy = 0; sy < s; sy++) {
                uint32_t dy = y + r * s + sy;
                if (dy >= fb->height) continue;
                uint32_t* row = (uint32_t*)(g_master_backbuffer + (uint64_t)dy * fb->pitch);
                for (int b = 0; b < 8; b++) {
                    if (glyph[r] & (1 << b)) {
                        for (uint32_t sx = 0; sx < s; sx++) {
                            uint32_t dx = x + b * s + sx;
                            if (dx < fb->width) row[dx] = color;
                        }
                    }
                }
            }
        }
    }
}

static inline void wm_draw_string(uint32_t x, uint32_t y, const char* s, uint32_t color, uint32_t scale_100 = 100) {
    uint32_t char_w = (8 * scale_100) / 100;
    for (int i = 0; s[i]; i++) {
        wm_draw_char(x + i * char_w, y, s[i], color, scale_100);
    }
}

static inline void wm_draw_window_chrome(int32_t x, int32_t y, int32_t w, int32_t h, const char* title, bool closable, int32_t min_y, int32_t max_y) {
    if (!g_master_backbuffer) return;
    // Title bar background (dark grey)
    for (int32_t ty = y; ty < y + (int32_t)g_title_h; ty++) {
        if (ty < 0 || (uint32_t)ty >= fb->height) continue;
        if (ty < min_y || ty > max_y) continue; // Respect dirty rect
        uint32_t* row = (uint32_t*)(g_master_backbuffer + (uint64_t)ty * fb->pitch);
        int32_t start_x = x; if (start_x < 0) start_x = 0;
        int32_t end_x = x + w; if (end_x > (int32_t)fb->width) end_x = fb->width;
        if (end_x > start_x) memset_32(row + start_x, 0xFF333333, end_x - start_x);
    }
    // Title text (Vertically centered)
    uint32_t char_h = (8 * g_font_scale_100) / 100;
    int32_t text_y = y + (int32_t)(g_title_h - char_h) / 2;
    if (text_y + (int32_t)char_h >= min_y && text_y <= max_y)
        wm_draw_string(x + 10, (uint32_t)text_y, title, 0xFFFFFFFF, g_font_scale_100);
    
    // Close button (Scaled with title bar)
    int32_t btn_pad = (int32_t)(4 * g_title_h) / 42;
    if (btn_pad < 2) btn_pad = 2;
    int32_t btn_h = (int32_t)g_title_h - btn_pad * 2;
    int32_t btn_w = btn_h;
    int32_t btn_x = x + w - btn_w - btn_pad;
    int32_t btn_y = y + btn_pad;

    uint32_t btn_clr = closable ? 0xFFFF0000 : 0xFF555555;
    for (int32_t by = btn_y; by < btn_y + btn_h; by++) {
        if (by < 0 || (uint32_t)by >= fb->height) continue;
        if (by < min_y || by > max_y) continue;
        uint32_t* row = (uint32_t*)(g_master_backbuffer + (uint64_t)by * fb->pitch);
        int32_t s_x = btn_x; if (s_x < 0) s_x = 0;
        int32_t e_x = btn_x + btn_w; if (e_x > (int32_t)fb->width) e_x = fb->width;
        if (e_x > s_x) memset_32(row + s_x, btn_clr, e_x - s_x);
    }
    // Centered 'X' in button
    int32_t x_off = (int32_t)(btn_w - char_h) / 2;
    int32_t y_off = (int32_t)(btn_h - char_h) / 2;
    if (btn_y + y_off >= min_y && btn_y + y_off + (int32_t)char_h <= max_y)
        wm_draw_char(btn_x + x_off, btn_y + y_off, 'X', 0xFFFFFFFF, g_font_scale_100);
}

// =============================================================================
//  OVERLAY LAYER — cursor + ghost drag outlines
//
//  These are painted DIRECTLY onto VRAM. master_backbuffer is ALWAYS
//  cursor-free. This decouples cursor speed from compositor cost entirely,
//  the same way a hardware sprite overlay works.
// =============================================================================

// Currently live on VRAM beyond master_backbuffer:
static int32_t ov_cx = -999, ov_cy = -999;
static bool    ov_ghost_term = false;
static int32_t ov_gt_x, ov_gt_y, ov_gt_w, ov_gt_h;
static bool    ov_ghost_test = false;
static int32_t ov_gs_x, ov_gs_y, ov_gs_w, ov_gs_h;
static bool    ov_ghost_rain = false;
static int32_t ov_gr_x, ov_gr_y, ov_gr_w, ov_gr_h;
static bool    ov_ghost_log  = false;
static int32_t ov_gl_x, ov_gl_y, ov_gl_w, ov_gl_h;

// Ghost drag: pending destination while LMB is held on a title bar.
// The real g_term_ox/oy don't change until mouse release — the window
// stays at its original position in master_backbuffer. Only the outline moves.
// This is the classic Win3.1 / X11 "move outline" mode.
static bool    wm_ghost_dragging_term  = false;
static bool    wm_ghost_dragging_test  = false;
static bool    wm_ghost_dragging_rain  = false;
static bool    wm_ghost_dragging_log   = false;
static int32_t wm_ghost_term_ox = 0, wm_ghost_term_oy = 0;
static int32_t wm_ghost_test_ox = 0, wm_ghost_test_oy = 0;
static int32_t wm_ghost_rain_ox = 0, wm_ghost_rain_oy = 0;
static int32_t wm_ghost_log_ox = 0, wm_ghost_log_oy = 0;
static int32_t wm_drag_term_offx = 0, wm_drag_term_offy = 0;
static int32_t wm_drag_test_offx = 0, wm_drag_test_offy = 0;
static int32_t wm_drag_rain_offx = 0, wm_drag_rain_offy = 0;
static int32_t wm_drag_log_offx = 0, wm_drag_log_offy = 0;

// ─── Low-level VRAM helpers ──────────────────────────────────────────────────

// Restore a rectangle in VRAM from the cursor-free master_backbuffer.
// This is the only "erase" we ever need for overlays.
static inline void vram_restore_rect(int32_t x1, int32_t y1,
                                      int32_t x2, int32_t y2) {
    if (!fb || !g_master_backbuffer) return;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if ((uint32_t)x2 >= fb->width)  x2 = (int32_t)fb->width  - 1;
    if ((uint32_t)y2 >= fb->height) y2 = (int32_t)fb->height - 1;
    if (x1 > x2 || y1 > y2) return;
    uint32_t bw = (uint32_t)(x2 - x1 + 1) * 4;
    for (int32_t y = y1; y <= y2; y++) {
        uint64_t off = (uint64_t)y * fb->pitch + (uint64_t)x1 * 4;
        memcpy_vram_sse_headless((uint8_t*)fb->address + off,
                                  g_master_backbuffer   + off, bw);
    }
}

// Erase only the 4 border strips of a ghost outline (not the interior).
// Much cheaper than restoring a full bounding box for large windows.
// Single top-to-bottom pass that erases the old outline and draws the new one
// simultaneously. Every scanline is fully settled (old pixels restored, new
// pixels written) before we advance, so the display can never catch a row
// where the old left/right border has vanished but the new one hasn't landed.
//
// has_old=false → draw only (no erase).  has_new=false → erase only.
// w/h are frame dimensions as used by vram_draw_outline (x2 = x + w - 1).
static inline void vram_transition_outline(
    int32_t ox, int32_t oy, int32_t ow, int32_t oh, bool has_old,
    int32_t nx, int32_t ny, int32_t nw, int32_t nh, bool has_new)
{
    if (!fb || !g_master_backbuffer) return;
    if (!has_old && !has_new) return;

    const uint32_t W = fb->width, H = fb->height;
    const uint32_t P = fb->pitch / 4;
    const uint32_t COL_OUTER = 0xFFFFFFFF, COL_INNER = 0xFF888888;

    // Right-edge pixel (vram_draw_outline uses x2 = x + w - 1)
    int32_t ox2 = ox + ow - 1, oy2 = oy + oh - 1;
    int32_t nx2 = nx + nw - 1, ny2 = ny + nh - 1;

    // Y range covering everything we need to touch
    int32_t y_lo = 0x7fffffff, y_hi = -0x7fffffff;
    if (has_old) { y_lo = oy - 2; y_hi = oy2 + 2; }
    if (has_new) {
        if (ny - 2 < y_lo) y_lo = ny - 2;
        if (ny2 + 2 > y_hi) y_hi = ny2 + 2;
    }
    if (y_lo < 0) y_lo = 0;
    if (y_hi >= (int32_t)H) y_hi = (int32_t)H - 1;
    if (y_lo > y_hi) return;

    for (int32_t y = y_lo; y <= y_hi; y++) {
        uint32_t* vrow = (uint32_t*)((uint8_t*)fb->address      + (uint64_t)y * fb->pitch);
        
        // ── Step 1: restore old outline's pixels on this scanline ────────────
        if (has_old) {
            bool on_top = (y >= oy - 2 && y <= oy + 3);
            bool on_bot = (y >= oy2 - 3 && y <= oy2 + 2);
            bool on_ls  = (y >= oy + 4 && y <= oy2 - 4);
            
            auto restore_strip = [&](int32_t ix1, int32_t ix2) {
                if (ix1 < 0) ix1 = 0; 
                if (ix2 >= (int32_t)W) ix2 = (int32_t)W - 1;
                if (ix1 <= ix2) {
                    uint64_t off = (uint64_t)y * fb->pitch + (uint64_t)ix1 * 4;
                    uint32_t len = (uint32_t)(ix2 - ix1 + 1) * 4;
                    // Use scalar memcpy for these tiny 24-byte strips; SSE overhead 
                    // and alignment quirks on small lengths can cause artifacts.
                    memcpy((uint8_t*)fb->address + off, g_master_backbuffer + off, len);
                }
            };

            if (on_top || on_bot) {
                restore_strip(ox - 2, ox2 + 2);
            } else if (on_ls) {
                // ONLY restore the left side and right side borders!
                restore_strip(ox - 2, ox + 3);
                restore_strip(ox2 - 3, ox2 + 2);
            }
        }

        // ── Step 2: draw new outline's pixels ────────────────────────────────
        if (has_new) {
            uint32_t clr = 0xFFFFFFFF;
            if (y >= ny && y <= ny + 1) { // Top hlines
                int32_t ix1 = nx, ix2 = nx + nw - 1;
                if (ix1 < 0) ix1 = 0; if (ix2 >= (int32_t)W) ix2 = (int32_t)W - 1;
                if (ix1 <= ix2) for (int32_t sx = ix1; sx <= ix2; sx++) vrow[sx] = clr;
            } else if (y >= ny2 - 2 && y <= ny2 - 1) { // Bottom hlines
                int32_t ix1 = nx, ix2 = nx + nw - 1;
                if (ix1 < 0) ix1 = 0; if (ix2 >= (int32_t)W) ix2 = (int32_t)W - 1;
                if (ix1 <= ix2) for (int32_t sx = ix1; sx <= ix2; sx++) vrow[sx] = clr;
            } else if (y >= ny && y < ny2) { // Vertical edges
                if (nx >= 0 && nx < (int32_t)W) { vrow[nx] = clr; if (nx + 1 < (int32_t)W) vrow[nx+1] = clr; }
                int32_t nx_r = nx + nw - 1;
                if (nx_r >= 0 && nx_r < (int32_t)W) { vrow[nx_r] = clr; if (nx_r - 1 >= 0) vrow[nx_r-1] = clr; }
            }
        }
    }
}

// Draw a 2px ghost outline rectangle directly to VRAM (not master_backbuffer).
static inline void vram_draw_outline(int32_t x, int32_t y,
                                      int32_t w, int32_t h) {
    if (!fb) return;
    uint32_t* vram = (uint32_t*)fb->address;
    uint32_t  P    = fb->pitch / 4;
    int32_t   x2   = x + w - 1, y2 = y + h - 1;

    const uint32_t outer = 0xFFFFFFFF, inner = 0xFF888888;

    auto safe_hline = [&](int32_t lx1, int32_t lx2, int32_t ly, uint32_t c) {
        if (ly < 0 || (uint32_t)ly >= fb->height) return;
        if (lx1 < 0) lx1 = 0;
        if ((uint32_t)lx2 >= fb->width) lx2 = (int32_t)fb->width - 1;
        uint32_t* row = vram + (uint64_t)ly * P;
        for (int32_t lx = lx1; lx <= lx2; lx++) row[lx] = c;
    };
    auto safe_vline = [&](int32_t lx, int32_t ly1, int32_t ly2, uint32_t c) {
        if (lx < 0 || (uint32_t)lx >= fb->width) return;
        for (int32_t ly = ly1; ly <= ly2; ly++) {
            if (ly < 0 || (uint32_t)ly >= fb->height) continue;
            vram[ly * P + lx] = c;
        }
    };

    safe_hline(x,   x2,   y,    outer); safe_hline(x+1, x2-1, y+1,  inner);
    safe_hline(x,   x2,   y2,   outer); safe_hline(x+1, x2-1, y2-1, inner);
    safe_vline(x,   y+2,  y2-2, outer); safe_vline(x+1, y+2,  y2-2, inner);
    safe_vline(x2,  y+2,  y2-2, outer); safe_vline(x2-1,y+2,  y2-2, inner);
}

// ─── Rainbow Window Generator ───────────────────────────────────────────────

static inline uint32_t wm_rainbow_color(uint32_t x) {
    uint32_t h = (x / 2) % 360;
    uint32_t s = 255, v = 255;
    uint32_t r, g, b;
    uint32_t i = h / 60;
    uint32_t f = h % 60;
    uint32_t p = (v * (255 - s)) >> 8;
    uint32_t q = (v * (255 * 60 - s * f)) / (255 * 60);
    uint32_t t = (v * (255 * 60 - s * (60 - f))) / (255 * 60);
    switch (i) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

static inline void wm_draw_rainbow_content(uint32_t* row, int32_t rw, uint32_t base_scroll, uint32_t offset_x) {
    uint32_t total = base_scroll + offset_x;
    uint32_t idx = (total / 2) % 360;
    uint32_t parity = total & 1;

    for (int32_t x = 0; x < rw; x++) {
        row[x] = g_rainbow_lut[idx];
        if (parity) {
            idx++;
            if (idx >= 360) idx = 0;
        }
        parity ^= 1;
    }
}

// Draw the cursor directly to VRAM. Does NOT modify master_backbuffer.
static inline void vram_draw_cursor(int32_t mx, int32_t my, uint8_t mb) {
    if (!fb) return;
    for (int cr = 0; cr < CURSOR_H; cr++) {
        int32_t sy = my + cr;
        if (sy < 0 || (uint32_t)sy >= fb->height) continue;
        uint32_t* row = (uint32_t*)((uint8_t*)fb->address + (uint64_t)sy * fb->pitch);
        for (int x = 0; x < CURSOR_W; x++) {
            int32_t sx = mx + x;
            if (sx < 0 || (uint32_t)sx >= fb->width) continue;
            uint8_t p = g_cursor_bitmap[cr][x];
            if (!p) continue;
            row[sx] = (p == 2)
                ? ((mb & 1) ? 0xFFFF0000 : (mb & 2) ? 0xFF00FFFF : 0xFF000000)
                : 0xFFFFFFFF;
        }
    }
}

// ─── Ghost frame geometry ────────────────────────────────────────────────────

static inline void ghost_term_frame(int32_t ox, int32_t oy,
                                     int32_t* fx, int32_t* fy,
                                     int32_t* fw, int32_t* fh) {
    uint32_t content_w = g_term_max_cols * 8;
    uint32_t content_h = g_term_max_rows * 10;
    *fx = ox - (int32_t)(g_border + g_pad);
    *fy = oy - (int32_t)(g_title_h + g_border + g_pad);
    *fw = (int32_t)content_w + (int32_t)(g_border + g_pad) * 2;
    *fh = (int32_t)content_h + (int32_t)(g_border * 2 + g_title_h + g_pad * 2);
}

static inline void ghost_test_frame(int32_t ox, int32_t oy,
                                     int32_t* fx, int32_t* fy,
                                     int32_t* fw, int32_t* fh) {
    *fx = ox; *fy = oy - (int32_t)g_title_h; *fw = test_w; *fh = test_h + (int32_t)g_title_h;
}

static inline void ghost_rain_frame(int32_t ox, int32_t oy,
                                     int32_t* fx, int32_t* fy,
                                     int32_t* fw, int32_t* fh) {
    *fx = ox; *fy = oy - (int32_t)g_title_h; *fw = rain_w; *fh = rain_h + (int32_t)g_title_h;
}

static inline void ghost_log_frame(int32_t ox, int32_t oy,
                                     int32_t* fx, int32_t* fy,
                                     int32_t* fw, int32_t* fh) {
    int32_t lw = (int32_t)g_log_win_cols * LOG_CHAR_W;
    int32_t lh = (int32_t)g_log_rows_visible * LOG_CHAR_H;
    *fx = ox - (int32_t)(g_border + g_pad);
    *fy = oy - (int32_t)(g_title_h + g_border + g_pad);
    *fw = lw + (int32_t)(g_border + g_pad) * 2;
    *fh = lh + (int32_t)(g_border * 2 + g_title_h + g_pad * 2);
}


// =============================================================================
//  wm_overlay_update — called on EVERY mouse event (no compositor involved)
//
//  Total pixel cost: ~12x20 cursor + 4x thin outline strips ≈ a few hundred px.
//  This is the entire cursor rendering budget, regardless of window size.
// =============================================================================
volatile bool g_vram_lock = false;

// =============================================================================
//  wm_overlay_update_unlocked
// =============================================================================
static inline void wm_overlay_update_unlocked(int32_t new_cx, int32_t new_cy, uint8_t mb) {
    if (!fb || !g_master_backbuffer) return;

    // Erase old cursor from VRAM
    vram_restore_rect(ov_cx - 1, ov_cy - 1,
                      ov_cx + CURSOR_W + 1, ov_cy + CURSOR_H + 1);

    // ── Terminal ghost: atomic erase-old + draw-new in one pass ─────────────
    {
        int32_t nfx = 0, nfy = 0, nfw = 0, nfh = 0;
        bool dn = wm_ghost_dragging_term;
        if (dn) ghost_term_frame(wm_ghost_term_ox, wm_ghost_term_oy,
                                 &nfx, &nfy, &nfw, &nfh);
        vram_transition_outline(ov_gt_x, ov_gt_y, ov_gt_w, ov_gt_h, ov_ghost_term,
                                nfx,     nfy,     nfw,     nfh,     dn);
        ov_ghost_term = dn;
        if (dn) { ov_gt_x = nfx; ov_gt_y = nfy; ov_gt_w = nfw; ov_gt_h = nfh; }
    }

    // ── Test ghost: same treatment ───────────────────────────────────────────
    {
        int32_t nfx = 0, nfy = 0, nfw = 0, nfh = 0;
        bool dn = wm_ghost_dragging_test;
        if (dn) ghost_test_frame(wm_ghost_test_ox, wm_ghost_test_oy,
                                 &nfx, &nfy, &nfw, &nfh);
        vram_transition_outline(ov_gs_x, ov_gs_y, ov_gs_w, ov_gs_h, ov_ghost_test,
                                nfx,     nfy,     nfw,     nfh,     dn);
        ov_ghost_test = dn;
        if (dn) { ov_gs_x = nfx; ov_gs_y = nfy; ov_gs_w = nfw; ov_gs_h = nfh; }
    }

    // ── Rainbow ghost ────────────────────────────────────────────────────────
    {
        int32_t nfx = 0, nfy = 0, nfw = 0, nfh = 0;
        bool dn = wm_ghost_dragging_rain;
        if (dn) ghost_rain_frame(wm_ghost_rain_ox, wm_ghost_rain_oy,
                                 &nfx, &nfy, &nfw, &nfh);
        vram_transition_outline(ov_gr_x, ov_gr_y, ov_gr_w, ov_gr_h, ov_ghost_rain,
                                nfx,     nfy,     nfw,     nfh,     dn);
        ov_ghost_rain = dn;
        if (dn) { ov_gr_x = nfx; ov_gr_y = nfy; ov_gr_w = nfw; ov_gr_h = nfh; }
    }

    // ── Log ghost ────────────────────────────────────────────────────────────
    {
        int32_t nfx = 0, nfy = 0, nfw = 0, nfh = 0;
        bool dn = wm_ghost_dragging_log;
        if (dn) ghost_log_frame(wm_ghost_log_ox, wm_ghost_log_oy,
                                 &nfx, &nfy, &nfw, &nfh);
        vram_transition_outline(ov_gl_x, ov_gl_y, ov_gl_w, ov_gl_h, ov_ghost_log,
                                nfx,     nfy,     nfw,     nfh,     dn);
        ov_ghost_log = dn;
        if (dn) { ov_gl_x = nfx; ov_gl_y = nfy; ov_gl_w = nfw; ov_gl_h = nfh; }
    }

    // Draw cursor on top of everything
    vram_draw_cursor(new_cx, new_cy, mb);
    ov_cx = new_cx; ov_cy = new_cy;

    vram_fence();
}

static inline void wm_overlay_update(int32_t new_cx, int32_t new_cy, uint8_t mb) {
    while (__sync_lock_test_and_set(&g_vram_lock, 1)) { __asm__ volatile("pause"); }
    wm_overlay_update_unlocked(new_cx, new_cy, mb);
    __sync_lock_release(&g_vram_lock);
}


// =============================================================================
//  wm_init
// =============================================================================
static inline void wm_init() {
    // 1080p Baseline: Laptop look is "Perfect" (1.0 scale)
    // MASTER SCALE: Apply height ratio to EVERYTHING to preserve aspect ratio.
    uint32_t sn = fb->height;
    uint32_t sd = 1080;

    g_taskbar_h = (sn * 60) / sd;
    g_title_h   = (sn * 42) / sd;
    g_border    = (sn * 3) / sd;
    g_pad       = (sn * 3) / sd;
    
    // Font scaling (1x for 1080p, 1.5x for 2K, 2x for 4K)
    g_font_scale_100 = (sn >= 2160) ? 200 : (sn >= 1440) ? 150 : 100;
    
    if (g_taskbar_h < 30) g_taskbar_h = 30;
    if (g_title_h < 20)   g_title_h = 20;
    if (g_border < 1)     g_border = 1;
    if (g_pad < 1)        g_pad = 1;
    
    // Horizontal elements use HEIGHT scale to maintain ratio
    g_sb_w   = (sn * 138) / sd;
    g_wbtn_w = (sn * 186) / sd;
    if (g_sb_w < 80)   g_sb_w = 80;
    if (g_wbtn_w < 110) g_wbtn_w = 110;

    // Content-First Window Sizes (Perfect 800x600 at 1080p)
    // 1. Calculate desired content area
    uint32_t desired_test_w = (sn * 800) / sd;
    uint32_t desired_test_h = (sn * 600) / sd;

    // 2. Snap to cell multiples to eliminate grey borders
    g_term_max_cols = desired_test_w / 8;
    g_term_max_rows = desired_test_h / 10;
    if (g_term_max_cols < 40) g_term_max_cols = 40;
    if (g_term_max_rows < 15) g_term_max_rows = 15;

    test_w = g_term_max_cols * 8;
    test_h = g_term_max_rows * 10;
    rain_w = test_w;
    rain_h = test_h;

    // 3. Scale Scroll window content too (UNIFY with terminal)
    g_log_win_cols = g_term_max_cols;
    g_log_rows_visible = g_term_max_rows;

    // Derived total window sizes
    wm_win_w = test_w + (g_border + g_pad) * 2;
    wm_win_h = test_h + g_border * 2 + g_title_h + g_pad * 2;
    
    // Center it
    wm_win_x = (fb->width > wm_win_w) ? (fb->width - wm_win_w) / 2 : 0;
    wm_win_y = (fb->height > wm_win_h) ? (fb->height - wm_win_h) / 2 : 0;

    g_term_ox = wm_win_x + g_border + g_pad;
    g_term_oy = wm_win_y + g_border + g_title_h + g_pad;

    wm_ghost_term_ox = (int32_t)g_term_ox;
    wm_ghost_term_oy = (int32_t)g_term_oy;
    wm_ghost_test_ox = test_x;
    wm_ghost_test_oy = test_y;
    wm_ghost_rain_ox = rain_x;
    wm_ghost_rain_oy = rain_y;

    for (int i = 0; i < 360; i++) {
        g_rainbow_lut[i] = wm_rainbow_color(i * 2);
    }

    // Note: Terminal state initialized in terminal.h / term_clear_screen
    if (g_backbuffer) memset_32(g_backbuffer, 0, (g_term_max_rows * 10) * (fb->pitch / 4));
    term_dirty_all();
}

// =============================================================================
//  wm_compose_dirty — CURSOR-FREE compositor
//
//  Builds the static scene into master_backbuffer and blits to VRAM.
//  Called by the 60Hz loop ONLY when scene content actually changes
//  (terminal text, scroll, window position committed after drag).
//  The cursor is NOT drawn here; wm_overlay_reapply() handles it after.
// =============================================================================
// Static region to backup pixels under overlays for inclusive staging
static uint32_t g_overlay_backup[256]; 

static inline int wm_find_top_window(int32_t mx, int32_t my) {
    for (int s = 3; s >= 0; s--) {
        int idx = g_win_order[s];
        if (!g_win_visible[idx]) continue;
        WinRect r; get_win_rect(idx, &r);
        if (mx >= r.x1 && mx < r.x2 && my >= r.y1 && my < r.y2) return idx;
    }
    return -1;
}

static inline void wm_raise_window(int idx) {
    int old_pos = -1;
    for (int i = 0; i < 4; i++) if (g_win_order[i] == idx) old_pos = i;
    if (old_pos == -1 || old_pos == 3) return;
    for (int i = old_pos; i < 3; i++) g_win_order[i] = g_win_order[i+1];
    g_win_order[3] = idx;
    g_dirty_min_x = 0; g_dirty_max_x = fb->width - 1;
    g_dirty_min_y = 0; g_dirty_max_y = fb->height - 1;
    g_needs_refresh = true;
}

static inline bool wm_is_in_title(int idx, int32_t mx, int32_t my) {
    WinRect r; get_win_rect(idx, &r);
    return (mx >= r.x1 && mx < r.x2 && my >= r.y1 && my < r.y1 + (int32_t)g_title_h);
}

static inline bool wm_is_in_close(int idx, int32_t mx, int32_t my) {
    WinRect r; get_win_rect(idx, &r);
    int32_t btn_x = r.x1 + (r.x2 - r.x1) - 24;
    int32_t btn_y = r.y1 + 4;
    return (mx >= btn_x && mx < btn_x + 20 && my >= btn_y && my < btn_y + 20);
}

static inline void wm_close_window(int idx) {
    if (idx == WIN_TERM) return; // Terminal is immortal
    g_win_visible[idx] = false;
    g_dirty_min_x = 0; g_dirty_max_x = fb->width - 1;
    g_dirty_min_y = 0; g_dirty_max_y = fb->height - 1;
    g_needs_refresh = true;
}

static inline void wm_compose_dirty(uint32_t min_x, uint32_t max_x,
                                     uint32_t min_y, uint32_t max_y) {
    if (!fb || !fb->address || !g_backbuffer || !g_master_backbuffer) return;
    if (min_x >= fb->width)  min_x = 0;
    if (max_x >= fb->width)  max_x = fb->width - 1;
    if (min_y >= fb->height) min_y = 0;
    if (max_y >= fb->height) max_y = fb->height - 1;
    if (min_y > max_y || min_x > max_x) return;

    WinRect rects[4];
    for (int i = 0; i < 4; i++) get_win_rect(i, &rects[i]);
    
    // Metrics reset (B/W/r/L are updated DURING the frame, V is from last)
    g_time_bg_ticks   = 0;
    g_time_win_ticks  = 0;
    g_time_rain_ticks = 0;
    g_time_lock_ticks = 0;

    uint64_t t0 = rdtsc_val();
    uint64_t t1, t2, t3;

    // Stage 1: Teal desktop (Hole-punched)
    for (uint32_t y = min_y; y <= max_y; y++) {
        uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)y * fb->pitch);
        if (y >= fb->height - g_taskbar_h) continue; // Taskbar area

        // Collect opaque spans for this scanline
        struct Span { int32_t x1, x2; };
        Span spans[4];
        int num_spans = 0;
        
        for (int i = 0; i < 4; i++) {
            if (rects[i].visible && (int32_t)y >= rects[i].y1 && (int32_t)y < rects[i].y2) {
                // Intersects this scanline
                int32_t sx1 = rects[i].x1; if (sx1 < (int32_t)min_x) sx1 = min_x;
                int32_t sx2 = rects[i].x2; if (sx2 > (int32_t)max_x + 1) sx2 = max_x + 1;
                if (sx1 < sx2) {
                    spans[num_spans].x1 = sx1;
                    spans[num_spans].x2 = sx2;
                    num_spans++;
                }
            }
        }
        
        // Sort spans by x1 (insertion sort is fine for n <= 3)
        for (int i = 1; i < num_spans; i++) {
            Span key = spans[i];
            int j = i - 1;
            while (j >= 0 && spans[j].x1 > key.x1) {
                spans[j + 1] = spans[j];
                j--;
            }
            spans[j + 1] = key;
        }

        // Draw the gaps between spans
        int32_t current_x = min_x;
        for (int i = 0; i < num_spans; i++) {
            if (current_x < spans[i].x1) {
                memset_32(drow + current_x, 0xFF008080, spans[i].x1 - current_x);
            }
            if (current_x < spans[i].x2) {
                current_x = spans[i].x2;
            }
        }
        
        // Draw any remaining background after the last span
        if (current_x <= (int32_t)max_x) {
            memset_32(drow + current_x, 0xFF008080, max_x - current_x + 1);
        }
    }
    
    t1 = rdtsc_val();
    g_time_bg_ticks = t1 - t0;

    // Stage 2: Windows in stack order
    for (int s = 0; s < 4; s++) {
        int idx = g_win_order[s];
        if (!g_win_visible[idx]) continue;
        WinRect r = rects[idx];

        int32_t dr1 = (r.y1 > (int32_t)min_y) ? r.y1 : (int32_t)min_y;
        int32_t dr2 = (r.y2 < (int32_t)max_y) ? r.y2 : (int32_t)max_y;
        // Don't draw windows over taskbar area
        if (dr2 > (int32_t)(fb->height - g_taskbar_h)) dr2 = fb->height - g_taskbar_h;
        if (dr1 >= dr2) continue;

        if (idx == WIN_TERM) {
            uint32_t wx = g_term_ox, wy = g_term_oy;
            uint32_t ww = g_term_max_cols * 8, wh = g_term_max_rows * 10;
            wm_draw_window_chrome(r.x1, r.y1, r.x2 - r.x1, r.y2 - r.y1, "Terminal", false, min_y, max_y);
            for (int32_t y = dr1; y < dr2; y++) {
                uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)y * fb->pitch);
                if (y >= r.y1 + g_title_h) {
                    int32_t s_x = r.x1; if (s_x < 0) s_x = 0;
                    int32_t e_x = r.x2; if (e_x > (int32_t)fb->width) e_x = fb->width;
                    if (e_x > s_x) {
                        bool in_text_rows = (y >= (int32_t)wy && y < (int32_t)(wy + wh));
                        bool in_text_cols_zone = ((int32_t)wx > s_x || e_x > (int32_t)(wx + ww));

                        if (!in_text_rows || !in_text_cols_zone) {
                            // Top padding strip (between titlebar and text), or bottom border:
                            // Fill the full row with gray.
                            memset_32(drow + s_x, 0xFFBBBBBB, e_x - s_x);
                        } else {
                            // In the text rows: only draw left and right chrome strips.
                            if ((int32_t)wx > s_x) memset_32(drow + s_x, 0xFFBBBBBB, wx - s_x);
                            int32_t w_end = wx + ww;
                            if (e_x > w_end) memset_32(drow + w_end, 0xFFBBBBBB, e_x - w_end);
                        }
                    }
                    if (y >= (int32_t)wy && y < (int32_t)(wy + wh)) {
                        int32_t ry = (int32_t)(y - wy);
                        memcpy_vram_sse_headless(drow + wx, (uint32_t*)(g_backbuffer + (uint64_t)ry * fb->pitch), ww * 4);
                    }
                }
            }
        } else if (idx == WIN_TEST) {
            wm_draw_window_chrome(r.x1, r.y1, (int32_t)test_w, (int32_t)test_h + (int32_t)g_title_h, "Visual Test", true, min_y, max_y);
            for (int32_t y = dr1; y < dr2; y++) {
                if (y < r.y1 + (int32_t)g_title_h) continue;
                uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)y * fb->pitch);
                int32_t s_x = r.x1; if (s_x < 0) s_x = 0;
                int32_t e_x = r.x2; if (e_x > (int32_t)fb->width) e_x = fb->width;
                if (e_x > s_x) {
                    for (int32_t cx = s_x; cx < e_x; ) {
                        uint32_t span_w = 50 - ((cx - r.x1) % 50);
                        if (cx + span_w > (uint32_t)e_x) span_w = e_x - cx;
                        uint32_t c = (((cx - r.x1) / 50) % 2 == ((y - (r.y1 + (int32_t)g_title_h)) / 50) % 2) ? 0xFFEEEEEE : 0xFFDDDDDD;
                        memset_32(drow + cx, c, span_w);
                        cx += span_w;
                    }
                }
            }
        } else if (idx == WIN_RAIN) {
            wm_draw_window_chrome(r.x1, r.y1, (int32_t)rain_w, (int32_t)rain_h + (int32_t)g_title_h, "Rainbow Animation", true, min_y, max_y);
            for (int32_t y = dr1; y < dr2; y++) {
                if (y < r.y1 + (int32_t)g_title_h) continue;
                uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)y * fb->pitch);
                int32_t s_x = r.x1; if (s_x < 0) s_x = 0;
                int32_t e_x = r.x2; if (e_x > (int32_t)fb->width) e_x = fb->width;
                if (e_x > s_x) wm_draw_rainbow_content(drow + s_x, e_x - s_x, rain_scroll, (uint32_t)(s_x - r.x1));
            }
        } else if (idx == WIN_LOG) {
            int32_t lw = (int32_t)g_log_win_cols * LOG_CHAR_W;
            int32_t lh = (int32_t)g_log_rows_visible * LOG_CHAR_H;
            wm_draw_window_chrome(r.x1, r.y1, lw + (int32_t)(g_border + g_pad) * 2, lh + (int32_t)(g_title_h + g_border * 2 + g_pad * 2), "Scroll Test", true, min_y, max_y); 

            for (int32_t y = dr1; y < dr2; y++) {
                uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)y * fb->pitch);
                if (y >= r.y1 + g_title_h) {
                    int32_t s_x = r.x1; if (s_x < 0) s_x = 0;
                    int32_t e_x = r.x2; if (e_x > (int32_t)fb->width) e_x = fb->width;
                    if (e_x > s_x) {
                        bool in_text_rows = (y >= log_win_y && y < log_win_y + lh);
                        bool in_text_cols_zone = ((int32_t)log_win_x > s_x || e_x > (int32_t)(log_win_x + lw));

                        if (!in_text_rows || !in_text_cols_zone) {
                            // Top padding strip or side/bottom borders: Fill with gray.
                            memset_32(drow + s_x, 0xFFBBBBBB, e_x - s_x);
                        } else {
                            // In the text rows: draw left/right gray strips, and the dark text background.
                            if ((int32_t)log_win_x > s_x) memset_32(drow + s_x, 0xFFBBBBBB, log_win_x - s_x);
                            int32_t w_end = log_win_x + lw;
                            if (e_x > w_end) memset_32(drow + w_end, 0xFFBBBBBB, e_x - w_end);
                            
                            // The actual dark text area
                            int32_t tx1 = log_win_x; if (tx1 < s_x) tx1 = s_x;
                            int32_t tx2 = log_win_x + lw; if (tx2 > e_x) tx2 = e_x;
                            if (tx1 < tx2) memset_32(drow + tx1, 0xFF0a0a14, tx2 - tx1);
                        }
                    }
                }
            }

            // Draw pre-rendered text area using fast SIMD memcpy
            uint32_t* lbb = g_log_win_backbuffer;
            if (lbb) {
                uint32_t stride = g_log_stride;
                uint32_t bw = g_log_win_cols * LOG_CHAR_W;
                uint32_t bh = g_log_rows_visible * LOG_CHAR_H;
                for (uint32_t r = 0; r < bh; r++) {
                    int32_t s_ry = log_win_y + r;
                    if (s_ry < (int32_t)min_y || s_ry > (int32_t)max_y) continue;
                    uint8_t* dst = g_master_backbuffer + (uint64_t)s_ry * fb->pitch + (uint64_t)log_win_x * 4;
                    uint8_t* src = (uint8_t*)(lbb + (uint64_t)r * stride);
                    memcpy_vram_sse_headless(dst, src, bw * 4);
                }
            }


        }
    }

    // Stage 2.5: Task Bar — Modern dark glass style
    int32_t tb_y = (int32_t)fb->height - (int32_t)g_taskbar_h;
    int32_t tbr1 = (tb_y > (int32_t)min_y) ? tb_y : (int32_t)min_y;
    int32_t tbr2 = (int32_t)max_y;
    if (tbr1 <= tbr2) {

        // ── Background gradient (deep navy, lighter at top) ──────────────────
        for (int32_t y = tbr1; y <= tbr2; y++) {
            if ((uint32_t)y >= fb->height) break;
            uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)y * fb->pitch);
            uint32_t row_off = (uint32_t)(y - tb_y);
            uint32_t clr;
            if      (row_off == 0) clr = 0xFF5a6490;   // bright shelf highlight
            else if (row_off == 1) clr = 0xFF303555;   // sub-highlight
            else {
                uint32_t t = ((row_off - 2) * 255) / (g_taskbar_h > 4 ? g_taskbar_h - 3 : 1);
                uint8_t r  = (uint8_t)(0x1c + (t * 4) / 255);
                uint8_t gc = (uint8_t)(0x20 + (t * 3) / 255);
                uint8_t bc = (uint8_t)(0x38 + (t * 5) / 255);
                clr = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)gc << 8) | bc;
            }
            memset_32(drow, clr, fb->width);
        }

        // ── Helper: gradient-filled button rect ─────────────────────────────
        // top_c/bot_c are the gradient endpoints; edge_c is the 1px border;
        // shine=true draws a bright highlight row on top (glass effect).
        auto draw_btn = [&](int32_t bx, int32_t by, int32_t bw, int32_t bh,
                            uint32_t top_c, uint32_t bot_c,
                            uint32_t edge_c, bool shine) {
            for (int32_t y = by; y < by + bh; y++) {
                if (y < (int32_t)min_y || y > (int32_t)max_y) continue;
                if (y < 0 || (uint32_t)y >= fb->height) continue;
                uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)y * fb->pitch);
                uint32_t t = (uint32_t)(y - by) * 255 / (bh > 1 ? (uint32_t)(bh - 1) : 1u);
                // Lerp each channel
                auto lerp8 = [](uint32_t a, uint32_t b, uint32_t t) -> uint8_t {
                    return (uint8_t)((int32_t)(a >> 16 & 0xFF) +
                        ((int32_t)(b >> 16 & 0xFF) - (int32_t)(a >> 16 & 0xFF)) * (int32_t)t / 255);
                };
                uint8_t rr  = (uint8_t)(((top_c>>16)&0xFF) + (int32_t)(((int32_t)((bot_c>>16)&0xFF)-(int32_t)((top_c>>16)&0xFF))*(int32_t)t/255));
                uint8_t gg  = (uint8_t)(((top_c>> 8)&0xFF) + (int32_t)(((int32_t)((bot_c>> 8)&0xFF)-(int32_t)((top_c>> 8)&0xFF))*(int32_t)t/255));
                uint8_t bb  = (uint8_t)(((top_c    )&0xFF) + (int32_t)(((int32_t)((bot_c    )&0xFF)-(int32_t)((top_c    )&0xFF))*(int32_t)t/255));
                uint32_t row_clr = 0xFF000000 | ((uint32_t)rr<<16) | ((uint32_t)gg<<8) | bb;
                if (shine && y == by) row_clr = 0xFFAABBEE;  // top shine row

                int32_t sx = bx < 0 ? 0 : bx;
                int32_t ex = bx + bw; if (ex > (int32_t)fb->width) ex = (int32_t)fb->width;
                if (ex > sx) memset_32(drow + sx, row_clr, (uint32_t)(ex - sx));

                // Left/right 1px edges
                if (bx >= 0 && (uint32_t)bx < fb->width)           drow[bx]       = edge_c;
                if (bx+bw-1 >= 0 && (uint32_t)(bx+bw-1) < fb->width) drow[bx+bw-1] = edge_c;
            }
            // Top / bottom edge lines
            for (int32_t px = bx; px < bx + bw; px++) {
                if (px < 0 || (uint32_t)px >= fb->width) continue;
                auto edge_px = [&](int32_t ey, uint32_t ec) {
                    if (ey < (int32_t)min_y || ey > (int32_t)max_y) return;
                    if (ey < 0 || (uint32_t)ey >= fb->height) return;
                    ((uint32_t*)(g_master_backbuffer + (uint64_t)ey * fb->pitch))[px] = ec;
                };
                edge_px(by,        edge_c);
                edge_px(by + bh - 1, 0xFF101828);
            }
        };

        // ── Start button ─────────────────────────────────────────────────────
        const int32_t PAD = 5;
        int32_t sb_x = 6,   sb_y = tb_y + PAD;
        int32_t sb_h = (int32_t)g_taskbar_h - PAD * 2;

        draw_btn(sb_x, sb_y, g_sb_w, sb_h,
                0xFF4a7ad8, 0xFF2a50b0,   // blue gradient
                0xFF1a3a90,               // dark blue border
                true);

        // "# START" label, centered
        const char* start_lbl   = "# START";
        const int32_t start_len = 7;
        uint32_t char_h = (8 * g_font_scale_100) / 100;
        uint32_t char_w = (8 * g_font_scale_100) / 100;
        wm_draw_string(sb_x + (int32_t)(g_sb_w - start_len * char_w) / 2,
                    sb_y + (int32_t)(sb_h - char_h) / 2,
                    start_lbl, 0xFFFFFFFF, g_font_scale_100);
        
        // (tray and separator code omitted for brevity in chunk but should stay same)
        // [wait, I need to make sure I don't delete them. I'll include them in the chunk.]

        // ── System tray (right side, drawn before task buttons so we know its x) 
        const int32_t TRAY_W = 80;
        int32_t tray_x = (int32_t)fb->width - TRAY_W - 6;
        int32_t tray_y = sb_y, tray_h = sb_h;

        draw_btn(tray_x, tray_y, TRAY_W, tray_h,
                0xFF252a42, 0xFF1a1f35,
                0xFF303560, false);
        wm_draw_string(tray_x + 6, tray_y + (tray_h - 8) / 2,
                    "12:00 PM", 0xFFBBBBDD);

        // Separator between task area and tray
        for (int32_t y = sb_y + 4; y < sb_y + sb_h - 4; y++) {
            if (y < (int32_t)min_y || y > (int32_t)max_y) continue;
            if (y < 0 || (uint32_t)y >= fb->height) continue;
            uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)y * fb->pitch);
            int32_t sep = tray_x - 6;
            if (sep >= 0 && (uint32_t)(sep+1) < fb->width) {
                drow[sep]   = 0xFF404870;
                drow[sep+1] = 0xFF1a1f35;
            }
        }

        // ── Window task buttons ───────────────────────────────────────────────
        const char* win_labels[4] = { "Terminal", "Visual Test", "Rainbow", "Scroll Test" };
        const int32_t WBTN_GAP = (int32_t)(4 * fb->height) / 1080;
        int32_t cur_x = sb_x + g_sb_w + (int32_t)(10 * fb->height) / 1080;
        int32_t active_idx = g_win_order[3];   // topmost window

        for (int idx = 0; idx < 4; idx++) {
            if (!g_win_visible[idx]) continue;
            bool active = (idx == active_idx);

            uint32_t top_c  = active ? 0xFF3d4d80 : 0xFF23283d;
            uint32_t bot_c  = active ? 0xFF28366a : 0xFF181c30;
            uint32_t edge_c = active ? 0xFF5568b0 : 0xFF2c3258;

            draw_btn(cur_x, sb_y, g_wbtn_w, sb_h, top_c, bot_c, edge_c, active);

            // Active indicator: 3px bright bar along the top edge
            if (active) {
                int32_t bar_y = sb_y;
                if (bar_y >= (int32_t)min_y && bar_y <= (int32_t)max_y &&
                    bar_y >= 0 && (uint32_t)bar_y < fb->height) {
                    uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)bar_y * fb->pitch);
                    int32_t bar_x1 = cur_x + 2, bar_x2 = cur_x + g_wbtn_w - 3;
                    if (bar_x1 < 0) bar_x1 = 0;
                    if (bar_x2 >= (int32_t)fb->width) bar_x2 = (int32_t)fb->width - 1;
                    if (bar_x2 > bar_x1) memset_32(drow + bar_x1, 0xFF6688FF, (uint32_t)(bar_x2 - bar_x1));
                }
            }

            // Window label, left-padded
            wm_draw_string(cur_x + 8, sb_y + (int32_t)(sb_h - char_h) / 2,
                        win_labels[idx],
                        active ? 0xFFEEEEFF : 0xFF8888AA,
                        g_font_scale_100);

            cur_x += g_wbtn_w + WBTN_GAP;
        }
    }

    // Stage 2.6: RAM display (top-right)
    {
        uint64_t total_mb = get_total_ram_count() / (1024 * 1024);
        uint64_t mapped_mb = get_mapped_ram_count() / (1024 * 1024);
        char m_str[16];
        char t_str[16];
        char ram_msg[64];
        
        to_str(mapped_mb, m_str);
        to_str(total_mb, t_str);
        
        char* d = ram_msg;
        const char* p;
        p = "RAM: "; while(*p) *d++ = *p++;
        p = m_str;   while(*p) *d++ = *p++;
        p = " MiB / "; while(*p) *d++ = *p++;
        p = t_str;   while(*p) *d++ = *p++;
        p = " MiB";   while(*p) *d++ = *p++;
        *d = '\0';
        
        uint32_t msg_len = strlen(ram_msg);
        uint32_t rx = fb->width - (msg_len * 8) - 20;
        uint32_t ry = 20;
        
        // Draw background only if within dirty rect
        if (ry + 12 >= min_y && ry - 4 <= max_y) {
            for (uint32_t by = ry - 6; by < ry + 14; by++) {
                if (by < min_y || by > max_y) continue;
                if (by >= fb->height) break;
                uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)by * fb->pitch);
                uint32_t bx = (rx > 6) ? rx - 6 : 0;
                uint32_t bw = msg_len * 8 + 12;
                if (bx + bw > fb->width) bw = fb->width - bx;
                memset_32(drow + bx, 0xCC1A1F35, bw); // Dark navy glass
            }
            wm_draw_string(rx, ry, ram_msg, 0xFFEEEEFF);
        }
    }

    // Stage 2.7: CPU display (below RAM)
    {
        // ── Task Count measurement ──────────────────────────────────────────
        static uint32_t g_task_count = 0;
        static uint64_t last_count_tick = 0;
        if (g_tick_count > last_count_tick + 500) { // Update tasks every 500ms
            last_count_tick = g_tick_count;
            uint32_t count = 0;
            extern Task* g_task_list;
            if (g_task_list) {
                Task* t = g_task_list;
                do {
                    count++;
                    t = t->next;
                } while (t != g_task_list);
            }
            g_task_count = count;
        }

        // ── CPU usage via TSC idle measurement ───────────────────────────────
        static uint64_t last_total_tsc  = 0;
        static uint64_t last_idle_tsc   = 0;
        static uint32_t g_cpu_usage_pct = 0;

        uint64_t now_tsc;
        __asm__ volatile("rdtsc; shl $32, %%rdx; or %%rdx, %%rax" : "=a"(now_tsc) :: "rdx");

        uint64_t cur_idle  = g_idle_tsc_accum;
        uint64_t elapsed   = now_tsc  - last_total_tsc;
        uint64_t idle_diff = cur_idle - last_idle_tsc;
        if (elapsed > 0 && elapsed < idle_diff) idle_diff = elapsed; // clamp

        if (elapsed > tsc_hz) { // Update exactly once per second
            uint64_t busy = (elapsed > idle_diff) ? (elapsed - idle_diff) : 0;
            g_cpu_usage_pct = (uint32_t)((busy * 100) / elapsed);
            last_total_tsc  = now_tsc;
            last_idle_tsc   = cur_idle;
        }

        // ── Extended Topology core count (Level 0xB) ────────────────────────
        static uint32_t g_vthr = 0, g_cores = 0;
        if (g_vthr == 0) {
            uint32_t eax, ebx, ecx, edx;
            __asm__ volatile("cpuid" : "=a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx) : "a"(0), "c"(0));
            uint32_t max_leaf = eax;
            bool got_topo = false;
            if (max_leaf >= 0xB) {
                __asm__ volatile("cpuid" : "=a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx) : "a"(0xB), "c"(0));
                uint32_t th_p_core = ebx & 0xFFFF;
                __asm__ volatile("cpuid" : "=a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx) : "a"(0xB), "c"(1));
                uint32_t tot_log = ebx & 0xFFFF;
                if (tot_log > 0 && th_p_core > 0) {
                    g_vthr = tot_log; g_cores = tot_log / th_p_core;
                    got_topo = true;
                }
            }
            if (!got_topo) {
                __asm__ volatile("cpuid" : "=a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx) : "a"(1), "c"(0));
                g_vthr = (ebx >> 16) & 0xFF; if (g_vthr == 0) g_vthr = 1;
                g_cores = g_vthr;
            }
        }

        // ── Task states & Frame time ──────────────────────────────────────────────
        char states_buf[64];
        char* sp = states_buf;
        *sp++ = ' '; *sp++ = '|'; *sp++ = ' ';
        if (g_task_list) {
            Task* t = g_task_list;
            int limit = 4;
            do {
                to_str(t->pid, sp); while(*sp) sp++; // append PID
                *sp++ = ':';
                if (t->state == TaskState::READY) *sp++ = 'R'; 
                else if (t->state == TaskState::BLOCKED) *sp++ = 'B';
                else if (t->state == TaskState::DEAD) *sp++ = 'D';
                else if (t->state == TaskState::RUNNING) *sp++ = 'H';
                else *sp++ = '?';
                *sp++ = ' ';
                t = t->next;
            } while (t != g_task_list && --limit > 0);
        }
        
        *sp++ = '|'; *sp++ = ' '; *sp++ = 'F'; *sp++ = ':';
        uint32_t frame_ms = 0;
        if (tsc_hz > 0) frame_ms = (uint32_t)((g_last_frame_ticks * 1000) / tsc_hz);
        to_str(frame_ms, sp); while(*sp) sp++;
        *sp++ = 'm'; *sp++ = 's';
        
        *sp++ = ' '; *sp++ = '('; *sp++ = 'B'; *sp++ = ':';
        to_str((uint32_t)(g_time_bg_ticks   * 1000 / tsc_hz), sp); while(*sp) sp++;
        *sp++ = ' '; *sp++ = 'W'; *sp++ = ':';
        to_str((uint32_t)(g_time_win_ticks  * 1000 / tsc_hz), sp); while(*sp) sp++;
        *sp++ = ' '; *sp++ = 'r'; *sp++ = ':';
        to_str((uint32_t)(g_time_rain_ticks * 1000 / tsc_hz), sp); while(*sp) sp++;
        *sp++ = ' '; *sp++ = 'L'; *sp++ = ':';
        to_str((uint32_t)(g_time_lock_ticks * 1000 / tsc_hz), sp); while(*sp) sp++;
        *sp++ = ' '; *sp++ = 'V'; *sp++ = ':';
        to_str((uint32_t)(g_time_vram_ticks * 1000 / tsc_hz), sp); while(*sp) sp++;
        *sp++ = ')'; *sp++ = ' ';
        *sp = '\0';

        // ── Build HUD string ────────────────────────────────────────────────
        char cpu_msg[128];
        char p1_str[8], tc_str[8], ut_str[8];
        to_str(g_cpu_usage_pct, p1_str);
        to_str(g_task_count, tc_str);
        to_str(g_tick_count / 1000, ut_str); 

        char* d = cpu_msg; const char* p;
        p = "Core0: "; while(*p) *d++ = *p++;
        p = p1_str;   while(*p) *d++ = *p++;
        p = "%";      while(*p) *d++ = *p++;
        p = states_buf; while(*p) *d++ = *p++;
        p = " | T:";   while(*p) *d++ = *p++;
        p = tc_str;   while(*p) *d++ = *p++;
        p = " | Up:";  while(*p) *d++ = *p++;
        p = ut_str;   while(*p) *d++ = *p++;
        p = "s";      while(*p) *d++ = *p++;
        *d = '\0';

        uint32_t msg_len = strlen(cpu_msg);
        uint32_t rx = fb->width - (msg_len * 8) - 20;
        uint32_t ry = 38; 

        if ((uint32_t)(ry + 12) >= min_y && ry >= 4 && (ry - 4) <= max_y) {
            for (uint32_t by = ry - 4; by < ry + 12; by++) {
                if (by < min_y || by > max_y) continue;
                if (by >= fb->height) break;
                uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)by * fb->pitch);
                uint32_t bx = (rx > 6) ? rx - 6 : 0;
                uint32_t bw = msg_len * 8 + 12;
                if (bx + bw > fb->width) bw = fb->width - bx;
                memset_32(drow + bx, 0xCC1A1F35, bw);
            }
            // Color: green when low usage, yellow when moderate, red when high
            uint32_t cpu_color = g_cpu_usage_pct < 50 ? 0xFF88FF88 :
                                 g_cpu_usage_pct < 80 ? 0xFFFFDD44 : 0xFFFF6644;
            wm_draw_string(rx, ry, cpu_msg, cpu_color);
        }
    }
    
    t2 = rdtsc_val();
    g_time_win_ticks = t2 - t1;

    // Stage 3: Blit dirty rectangle to VRAM
    if (min_x > max_x || min_y > max_y) {
        vram_fence();
        return;
    }
    uint32_t ptch = fb->pitch;
    uint32_t bw   = (max_x - min_x + 1) * 4;

    uint64_t lt0 = rdtsc_val();
    while (__sync_lock_test_and_set(&g_vram_lock, 1)) { __asm__ volatile("pause"); }
    g_time_lock_ticks = rdtsc_val() - lt0;

    // ── Compose cursor into master_backbuffer before blit ──────────────────
    // This ensures the cursor is ALWAYS included in the blit and there is
    // never a frame where the cursor is missing from VRAM.
    // We save the affected pixels first so we can restore backbuffer after.
    int32_t ccx = g_mouse_x, ccy = g_mouse_y;
    uint8_t cmb = g_mouse_buttons;
    static uint32_t cursor_save_buf[CURSOR_H * CURSOR_W];
    uint32_t pitch32 = ptch / 4;
    for (int cr = 0; cr < CURSOR_H; cr++) {
        int32_t sy = ccy + cr;
        if (sy < 0 || (uint32_t)sy >= fb->height) {
            for (int x = 0; x < CURSOR_W; x++) cursor_save_buf[cr * CURSOR_W + x] = 0;
            continue;
        }
        uint32_t* bbrow = (uint32_t*)(g_master_backbuffer + (uint64_t)sy * ptch);
        for (int x = 0; x < CURSOR_W; x++) {
            int32_t sx = ccx + x;
            if (sx < 0 || (uint32_t)sx >= fb->width) { cursor_save_buf[cr * CURSOR_W + x] = 0; continue; }
            cursor_save_buf[cr * CURSOR_W + x] = bbrow[sx]; // save
            uint8_t p = g_cursor_bitmap[cr][x];
            if (p) bbrow[sx] = (p == 2)
                ? ((cmb & 1) ? 0xFFFF0000 : (cmb & 2) ? 0xFF00FFFF : 0xFF000000)
                : 0xFFFFFFFF; // stamp
        }
    }

    // ── Blit dirty rect (64-byte aligned for max throughput) ───────────────
    uint32_t min_x_aligned = min_x & ~15u; // 16-byte align for SSE (64 would be safer but might over-copy too much)
    uint32_t max_x_aligned = (max_x + 15) & ~15u;
    if (max_x_aligned > fb->width) max_x_aligned = fb->width;
    uint32_t bw_aligned = (max_x_aligned - min_x_aligned) * 4;

    for (uint32_t y = min_y; y <= max_y; y++) {
        uint8_t* dst = (uint8_t*)fb->address + (uint64_t)y * ptch + (uint64_t)min_x_aligned * 4;
        uint8_t* src = g_master_backbuffer   + (uint64_t)y * ptch + (uint64_t)min_x_aligned * 4;
        memcpy_vram_sse_headless(dst, src, bw_aligned);
    }
    __asm__ volatile("sfence" : : : "memory");
    t3 = rdtsc_val();
    
    g_time_bg_ticks   = t1 - t0;
    g_time_win_ticks  = t2 - t1;
    g_time_vram_ticks = t3 - t2;

    // ── Restore backbuffer cursor pixels ────────────────────────────────────
    // (master_backbuffer does not own cursor pixels; VRAM does)
    for (int cr = 0; cr < CURSOR_H; cr++) {
        int32_t sy = ccy + cr;
        if (sy < 0 || (uint32_t)sy >= fb->height) continue;
        uint32_t* bbrow = (uint32_t*)(g_master_backbuffer + (uint64_t)sy * ptch);
        for (int x = 0; x < CURSOR_W; x++) {
            int32_t sx = ccx + x;
            if (sx < 0 || (uint32_t)sx >= fb->width) continue;
            if (g_cursor_bitmap[cr][x]) bbrow[sx] = cursor_save_buf[cr * CURSOR_W + x];
        }
    }

    // Draw ghost outlines to VRAM and update cursor position tracking.
    // Ghost outlines are direct-VRAM-only (not baked into backbuffer).
    // NOTE: do NOT set ov_cx/ov_cy here — wm_overlay_update_unlocked needs
    // the OLD values to erase the previous cursor position from VRAM.
    wm_overlay_update_unlocked(ccx, ccy, cmb);

    __sync_lock_release(&g_vram_lock);
}

#endif // WM_H