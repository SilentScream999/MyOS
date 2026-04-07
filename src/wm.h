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

#define WM_TITLE_H  28
#define WM_BORDER    2
#define WM_PAD       2
#define CURSOR_W    12
#define CURSOR_H    19

#define WIN_TERM 0
#define WIN_TEST 1
#define WIN_RAIN 2

static int  g_win_order[3]   = {WIN_TERM, WIN_TEST, WIN_RAIN};
static bool g_win_visible[3] = {true, true, true};

static int32_t  test_x = 450, test_y = 120, test_w = 800, test_h = 600;
static int32_t  rain_x = 150, rain_y = 420, rain_w = 800, rain_h = 600;
static uint32_t rain_scroll = 0;
static uint32_t g_rainbow_lut[360];
static uint32_t wm_win_x, wm_win_y, wm_win_w, wm_win_h;
static uint32_t g_taskbar_h = 40;

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
        r->x1 = (wx > (uint32_t)(WM_BORDER + WM_PAD)) ? wx - WM_BORDER - WM_PAD : 0;
        r->x2 = wx + ww + WM_BORDER + WM_PAD;
        r->y1 = (wy > (uint32_t)(WM_BORDER + WM_TITLE_H + WM_PAD)) ? (int32_t)(wy - WM_BORDER - WM_TITLE_H - WM_PAD) : 0;
        r->y2 = (int32_t)(wy + wh + WM_BORDER + WM_PAD);
    } else if (idx == WIN_TEST) {
        r->x1 = test_x; r->y1 = test_y - 28; r->x2 = test_x + test_w; r->y2 = test_y + test_h;
    } else if (idx == WIN_RAIN) {
        r->x1 = rain_x; r->y1 = rain_y - 28; r->x2 = rain_x + rain_w; r->y2 = rain_y + rain_h;
    }
}

static inline void wm_draw_char(uint32_t x, uint32_t y, char c, uint32_t color) {
    if (!g_master_backbuffer || (uint32_t)c > 127) return;
    uint8_t* glyph = (uint8_t*)font8x8_basic[(uint8_t)c];
    for (int r = 0; r < 8; r++) {
        uint32_t ry = y + r;
        if (ry >= fb->height) continue;
        uint32_t* row = (uint32_t*)(g_master_backbuffer + (uint64_t)ry * fb->pitch);
        for (int b = 0; b < 8; b++) {
            if (glyph[r] & (1 << b)) {
                uint32_t rx = x + b;
                if (rx < fb->width) row[rx] = color;
            }
        }
    }
}

static inline void wm_draw_string(uint32_t x, uint32_t y, const char* s, uint32_t color) {
    for (int i = 0; s[i]; i++) {
        wm_draw_char(x + i * 8, y, s[i], color);
    }
}

static inline void wm_draw_window_chrome(int32_t x, int32_t y, int32_t w, int32_t h, const char* title, bool closable, int32_t min_y, int32_t max_y) {
    if (!g_master_backbuffer) return;
    // Title bar background (dark grey)
    for (int32_t ty = y; ty < y + WM_TITLE_H; ty++) {
        if (ty < 0 || (uint32_t)ty >= fb->height) continue;
        if (ty < min_y || ty > max_y) continue; // Respect dirty rect
        uint32_t* row = (uint32_t*)(g_master_backbuffer + (uint64_t)ty * fb->pitch);
        int32_t start_x = x; if (start_x < 0) start_x = 0;
        int32_t end_x = x + w; if (end_x > (int32_t)fb->width) end_x = fb->width;
        if (end_x > start_x) memset_32(row + start_x, 0xFF333333, end_x - start_x);
    }
    // Title text
    if (y + 10 >= min_y && y + 18 <= max_y)
        wm_draw_string(x + 10, y + 10, title, 0xFFFFFFFF);
    
    // Close button
    int32_t btn_x = x + w - 24, btn_y = y + 4, btn_w = 20, btn_h = 20;
    uint32_t btn_clr = closable ? 0xFFFF0000 : 0xFF555555;
    for (int32_t by = btn_y; by < btn_y + btn_h; by++) {
        if (by < 0 || (uint32_t)by >= fb->height) continue;
        if (by < min_y || by > max_y) continue;
        uint32_t* row = (uint32_t*)(g_master_backbuffer + (uint64_t)by * fb->pitch);
        int32_t s_x = btn_x; if (s_x < 0) s_x = 0;
        int32_t e_x = btn_x + btn_w; if (e_x > (int32_t)fb->width) e_x = fb->width;
        if (e_x > s_x) memset_32(row + s_x, btn_clr, e_x - s_x);
    }
    if (btn_y + 6 >= min_y && btn_y + 14 <= max_y)
        wm_draw_char(btn_x + 6, btn_y + 6, 'X', 0xFFFFFFFF);
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

// Ghost drag: pending destination while LMB is held on a title bar.
// The real g_term_ox/oy don't change until mouse release — the window
// stays at its original position in master_backbuffer. Only the outline moves.
// This is the classic Win3.1 / X11 "move outline" mode.
static bool    wm_ghost_dragging_term  = false;
static bool    wm_ghost_dragging_test  = false;
static bool    wm_ghost_dragging_rain  = false;
static int32_t wm_ghost_term_ox = 0, wm_ghost_term_oy = 0;
static int32_t wm_ghost_test_ox = 0, wm_ghost_test_oy = 0;
static int32_t wm_ghost_rain_ox = 0, wm_ghost_rain_oy = 0;
static int32_t wm_drag_term_offx = 0, wm_drag_term_offy = 0;
static int32_t wm_drag_test_offx = 0, wm_drag_test_offy = 0;
static int32_t wm_drag_rain_offx = 0, wm_drag_rain_offy = 0;

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
            
            if (on_top || on_bot || on_ls) {
                int32_t ix1 = ox - 2, ix2 = ox2 + 2;
                if (ix1 < 0) ix1 = 0; if (ix2 >= (int32_t)W) ix2 = (int32_t)W - 1;
                if (ix1 <= ix2) {
                    uint64_t off = (uint64_t)y * fb->pitch + (uint64_t)ix1 * 4;
                    uint32_t len = (uint32_t)(ix2 - ix1 + 1) * 4;
                    memcpy_vram_sse_headless((uint8_t*)fb->address + off, g_master_backbuffer + off, len);
                }
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

static inline void wm_draw_rainbow_content(uint32_t* row, int32_t rw, uint32_t scroll) {
    uint32_t s = scroll / 2;
    for (int32_t x = 0; x < rw; x++) {
        row[x] = g_rainbow_lut[(s + x/2) % 360];
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
    *fw = (int32_t)(g_term_max_cols * 8) + (WM_BORDER + WM_PAD) * 2;
    *fh = (int32_t)(g_term_max_rows * 10) + WM_BORDER * 2 + WM_TITLE_H + WM_PAD * 2;
    *fx = ox - (WM_BORDER + WM_PAD);
    *fy = oy - (WM_BORDER + WM_TITLE_H + WM_PAD);
}

static inline void ghost_test_frame(int32_t ox, int32_t oy,
                                     int32_t* fx, int32_t* fy,
                                     int32_t* fw, int32_t* fh) {
    *fx = ox; *fy = oy - 28; *fw = test_w; *fh = test_h + 28;
}

static inline void ghost_rain_frame(int32_t ox, int32_t oy,
                                     int32_t* fx, int32_t* fy,
                                     int32_t* fw, int32_t* fh) {
    *fx = ox; *fy = oy - 28; *fw = rain_w; *fh = rain_h + 28;
}

// =============================================================================
//  wm_overlay_update — called on EVERY mouse event (no compositor involved)
//
//  Total pixel cost: ~12x20 cursor + 4x thin outline strips ≈ a few hundred px.
//  This is the entire cursor rendering budget, regardless of window size.
// =============================================================================
static inline void wm_overlay_update(int32_t new_cx, int32_t new_cy, uint8_t mb) {
    if (!fb || !g_master_backbuffer) return;

    // Erase old cursor
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

    // Draw cursor on top of everything
    vram_draw_cursor(new_cx, new_cy, mb);
    ov_cx = new_cx; ov_cy = new_cy;

    vram_fence();
}

// Called by wm_compose_dirty after blitting the clean scene to VRAM,
// to re-stamp the overlay on top.
static inline void wm_overlay_reapply(int32_t cx, int32_t cy, uint8_t mb) {
    if (!fb) return;
    
    // Stamp current ghost outlines
    if (wm_ghost_dragging_term) {
        int32_t fx, fy, fw, fh;
        ghost_term_frame(wm_ghost_term_ox, wm_ghost_term_oy, &fx, &fy, &fw, &fh);
        vram_draw_outline(fx, fy, fw, fh);
    }
    if (wm_ghost_dragging_test) {
        int32_t fx, fy, fw, fh;
        ghost_test_frame(wm_ghost_test_ox, wm_ghost_test_oy, &fx, &fy, &fw, &fh);
        vram_draw_outline(fx, fy, fw, fh);
    }
    if (wm_ghost_dragging_rain) {
        int32_t fx, fy, fw, fh;
        ghost_rain_frame(wm_ghost_rain_ox, wm_ghost_rain_oy, &fx, &fy, &fw, &fh);
        vram_draw_outline(fx, fy, fw, fh);
    }

    vram_draw_cursor(cx, cy, mb);
    vram_fence();
}

// =============================================================================
//  wm_init
// =============================================================================
static inline void wm_init() {
    uint32_t margin = fb->width / 10;
    wm_win_x = margin;
    wm_win_y = margin / 2;
    wm_win_w = fb->width  - margin * 2;
    wm_win_h = fb->height - margin;

    g_term_ox = wm_win_x + WM_BORDER + WM_PAD;
    g_term_oy = wm_win_y + WM_BORDER + WM_TITLE_H + WM_PAD;
    g_term_max_cols = (wm_win_w - (WM_BORDER + WM_PAD) * 2) / 8;
    g_term_max_rows = (wm_win_h -  WM_BORDER * 2 - WM_TITLE_H - WM_PAD * 2) / 10;

    wm_ghost_term_ox = (int32_t)g_term_ox;
    wm_ghost_term_oy = (int32_t)g_term_oy;
    wm_ghost_test_ox = test_x;
    wm_ghost_test_oy = test_y;
    wm_ghost_rain_ox = rain_x;
    wm_ghost_rain_oy = rain_y;

    for (int i = 0; i < 360; i++) {
        g_rainbow_lut[i] = wm_rainbow_color(i * 2);
    }

    g_voffset = 0; g_view_delta = 0;
    if (g_backbuffer) memset(g_backbuffer, 0, g_term_buf_height * fb->pitch);
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
    for (int s = 2; s >= 0; s--) {
        int idx = g_win_order[s];
        if (!g_win_visible[idx]) continue;
        WinRect r; get_win_rect(idx, &r);
        if (mx >= r.x1 && mx < r.x2 && my >= r.y1 && my < r.y2) return idx;
    }
    return -1;
}

static inline void wm_raise_window(int idx) {
    int old_pos = -1;
    for (int i = 0; i < 3; i++) if (g_win_order[i] == idx) old_pos = i;
    if (old_pos == -1 || old_pos == 2) return;
    for (int i = old_pos; i < 2; i++) g_win_order[i] = g_win_order[i+1];
    g_win_order[2] = idx;
    g_dirty_min_x = 0; g_dirty_max_x = fb->width - 1;
    g_dirty_min_y = 0; g_dirty_max_y = fb->height - 1;
    g_needs_refresh = true;
}

static inline bool wm_is_in_title(int idx, int32_t mx, int32_t my) {
    WinRect r; get_win_rect(idx, &r);
    return (mx >= r.x1 && mx < r.x2 && my >= r.y1 && my < r.y1 + WM_TITLE_H);
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

    WinRect rects[3];
    for (int i = 0; i < 3; i++) get_win_rect(i, &rects[i]);

    // Stage 1: Teal desktop (Hole-punched)
    for (uint32_t y = min_y; y <= max_y; y++) {
        uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)y * fb->pitch);
        if (y >= fb->height - g_taskbar_h) continue; // Taskbar area

        for (uint32_t x = min_x; x <= max_x; ) {
            bool in_any = false;
            for (int i = 0; i < 3; i++) {
                if (rects[i].visible && (int32_t)y >= rects[i].y1 && (int32_t)y < rects[i].y2 &&
                    (int32_t)x >= rects[i].x1 && (int32_t)x < rects[i].x2) { in_any = true; break; }
            }
            if (in_any) { x++; continue; }
            
            uint32_t start_x = x;
            while (x <= max_x) {
                if (x >= fb->width) break;
                bool next_in_any = false;
                for (int i = 0; i < 3; i++) {
                    if (rects[i].visible && (int32_t)y >= rects[i].y1 && (int32_t)y < rects[i].y2 &&
                        (int32_t)x >= rects[i].x1 && (int32_t)x < rects[i].x2) { next_in_any = true; break; }
                }
                if (next_in_any) break;
                x++;
            }
            if (x > start_x) memset_32(drow + start_x, 0xFF008080, x - start_x);
        }
    }

    // Stage 2: Windows in stack order
    for (int s = 0; s < 3; s++) {
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
                if (y >= r.y1 + 28) {
                    int32_t s_x = r.x1; if (s_x < 0) s_x = 0;
                    int32_t e_x = r.x2; if (e_x > (int32_t)fb->width) e_x = fb->width;
                    if (e_x > s_x) memset_32(drow + s_x, 0xFFBBBBBB, e_x - s_x);
                    if (y >= (int32_t)wy && y < (int32_t)(wy + wh)) {
                        int32_t ry = (int32_t)(y - wy + g_voffset) + g_view_delta * 10;
                        ry = ((ry % (int32_t)g_term_buf_height) + (int32_t)g_term_buf_height) % (int32_t)g_term_buf_height;
                        memcpy(drow + wx, (uint32_t*)(g_backbuffer + (uint64_t)ry * fb->pitch), ww * 4);
                    }
                }
            }
        } else if (idx == WIN_TEST) {
            wm_draw_window_chrome(r.x1, r.y1, test_w, test_h + 28, "Visual Test", true, min_y, max_y);
            for (int32_t y = dr1; y < dr2; y++) {
                if (y < r.y1 + 28) continue;
                uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)y * fb->pitch);
                int32_t s_x = r.x1; if (s_x < 0) s_x = 0;
                int32_t e_x = r.x2; if (e_x > (int32_t)fb->width) e_x = fb->width;
                if (e_x > s_x) {
                    for (int32_t cx = s_x; cx < e_x; ) {
                        uint32_t span_w = 50 - ((cx - r.x1) % 50);
                        if (cx + span_w > (uint32_t)e_x) span_w = e_x - cx;
                        uint32_t c = (((cx - r.x1) / 50) % 2 == (y / 50) % 2) ? 0xFFEEEEEE : 0xFFDDDDDD;
                        memset_32(drow + cx, c, span_w);
                        cx += span_w;
                    }
                }
            }
        } else if (idx == WIN_RAIN) {
            wm_draw_window_chrome(r.x1, r.y1, rain_w, rain_h + 28, "Rainbow Animation", true, min_y, max_y);
            for (int32_t y = dr1; y < dr2; y++) {
                if (y < r.y1 + 28) continue;
                uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)y * fb->pitch);
                int32_t s_x = r.x1; if (s_x < 0) s_x = 0;
                int32_t e_x = r.x2; if (e_x > (int32_t)fb->width) e_x = fb->width;
                if (e_x > s_x) wm_draw_rainbow_content(drow + s_x, e_x - s_x, rain_scroll + (s_x - r.x1));
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
        int32_t sb_w = 92,  sb_h = (int32_t)g_taskbar_h - PAD * 2;

        draw_btn(sb_x, sb_y, sb_w, sb_h,
                0xFF4a7ad8, 0xFF2a50b0,   // blue gradient
                0xFF1a3a90,               // dark blue border
                true);

        // "# START" label, centered
        const char* start_lbl   = "# START";
        const int32_t start_len = 7;
        wm_draw_string(sb_x + (sb_w - start_len * 8) / 2,
                    sb_y + (sb_h - 8) / 2,
                    start_lbl, 0xFFFFFFFF);

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
        const char* win_labels[3] = { "Terminal", "Visual Test", "Rainbow" };
        const int32_t WBTN_W = 124, WBTN_GAP = 4;
        int32_t cur_x = sb_x + sb_w + 10;
        int32_t active_idx = g_win_order[2];   // topmost window

        for (int idx = 0; idx < 3; idx++) {
            if (!g_win_visible[idx]) continue;
            bool active = (idx == active_idx);

            uint32_t top_c  = active ? 0xFF3d4d80 : 0xFF23283d;
            uint32_t bot_c  = active ? 0xFF28366a : 0xFF181c30;
            uint32_t edge_c = active ? 0xFF5568b0 : 0xFF2c3258;

            draw_btn(cur_x, sb_y, WBTN_W, sb_h, top_c, bot_c, edge_c, active);

            // Active indicator: 3px bright bar along the top edge
            if (active) {
                int32_t bar_y = sb_y;
                if (bar_y >= (int32_t)min_y && bar_y <= (int32_t)max_y &&
                    bar_y >= 0 && (uint32_t)bar_y < fb->height) {
                    uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)bar_y * fb->pitch);
                    int32_t bar_x1 = cur_x + 2, bar_x2 = cur_x + WBTN_W - 3;
                    if (bar_x1 < 0) bar_x1 = 0;
                    if (bar_x2 >= (int32_t)fb->width) bar_x2 = (int32_t)fb->width - 1;
                    if (bar_x2 > bar_x1) memset_32(drow + bar_x1, 0xFF6688FF, (uint32_t)(bar_x2 - bar_x1));
                }
            }

            // Window label, left-padded
            wm_draw_string(cur_x + 8, sb_y + (sb_h - 8) / 2,
                        win_labels[idx],
                        active ? 0xFFEEEEFF : 0xFF8888AA);

            cur_x += WBTN_W + WBTN_GAP;
        }
    }

    // Stage 3: Blit dirty rectangle to VRAM
    uint32_t ptch = fb->pitch;
    uint32_t bw   = (max_x - min_x + 1) * 4;
    int32_t g1x, g1y, g1w, g1h; bool h1 = false;
    int32_t g2x, g2y, g2w, g2h; bool h2 = false;
    int32_t g3x, g3y, g3w, g3h; bool h3 = false;
    int32_t cx = g_mouse_x, cy = g_mouse_y;
    uint8_t mb = g_mouse_buttons;
    if (wm_ghost_dragging_term) { ghost_term_frame(wm_ghost_term_ox, wm_ghost_term_oy, &g1x, &g1y, &g1w, &g1h); h1 = true; }
    if (wm_ghost_dragging_test) { ghost_test_frame(wm_ghost_test_ox, wm_ghost_test_oy, &g2x, &g2y, &g2w, &g2h); h2 = true; }
    if (wm_ghost_dragging_rain) { ghost_rain_frame(wm_ghost_rain_ox, wm_ghost_rain_oy, &g3x, &g3y, &g3w, &g3h); h3 = true; }

    for (uint32_t y = min_y; y <= max_y; y++) {
        uint8_t* dst = (uint8_t*)fb->address + (uint64_t)y * ptch + (uint64_t)min_x * 4;
        uint8_t* src = g_master_backbuffer   + (uint64_t)y * ptch + (uint64_t)min_x * 4;
        memcpy_vram_sse_headless(dst, src, bw);
        __asm__ volatile("sfence" ::: "memory");
        uint32_t* vrow = (uint32_t*)((uint8_t*)fb->address + (uint64_t)y * fb->pitch);
        auto draw_ghost_row = [&](int32_t gx, int32_t gy, int32_t gw, int32_t gh) {
            if ((int32_t)y < gy || (int32_t)y >= gy + gh) return;
            uint32_t clr = 0xFFFFFFFF;
            int32_t gy2 = gy + gh;
            if ((int32_t)y == gy || (int32_t)y == gy + 1 || (int32_t)y == gy2 - 1 || (int32_t)y == gy2 - 2) {
                for (int32_t sx = gx; sx < gx + gw; sx++) if (sx >= 0 && (uint32_t)sx < fb->width) vrow[sx] = clr;
            } else {
                if (gx >= 0 && (uint32_t)gx < fb->width) { vrow[gx] = clr; if (gx + 1 < (int32_t)fb->width) vrow[gx+1] = clr; }
                if (gx + gw - 1 >= 0 && (uint32_t)(gx + gw - 1) < fb->width) { vrow[gx+gw-1] = clr; if (gx+gw-2 >= 0) vrow[gx+gw-2] = clr; }
            }
        };
        if (h1) draw_ghost_row(g1x, g1y, g1w, g1h);
        if (h2) draw_ghost_row(g2x, g2y, g2w, g2h);
        if (h3) draw_ghost_row(g3x, g3y, g3w, g3h);
        if ((int32_t)y >= cy && (int32_t)y < cy + CURSOR_H) {
            int cr = (int)y - cy;
            for (int x = 0; x < CURSOR_W; x++) {
                int32_t sx = cx + x;
                if (sx < 0 || (uint32_t)sx >= fb->width) continue;
                uint8_t p = g_cursor_bitmap[cr][x];
                if (!p) continue;
                vrow[sx] = (p == 2) ? ((mb & 1) ? 0xFFFF0000 : (mb & 2) ? 0xFF00FFFF : 0xFF000000) : 0xFFFFFFFF;
            }
        }
    }
    vram_fence();
    wm_overlay_reapply(cx, cy, mb);
    ov_cx = cx; ov_cy = cy;
}

#endif // WM_H