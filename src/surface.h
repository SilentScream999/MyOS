#ifndef surface_h
#define surface_h

// ─── surface.h ──────────────────────────────────────────────────────────────
// Window-surface model, step 1.
//
//   * A window owns a SURFACE: a pixel buffer (win->buffer, pitch win->buffer_pitch)
//     covering its client area, origin (0,0) = top-left of the client area.
//   * A client (an app, or a built-in like the rainbow) draws ONLY into its own
//     surface and then reports DAMAGE: the rectangle of the surface that changed.
//   * The compositor knows nothing about what any client is. It turns damage into
//     a screen dirty-rectangle and composes every window the same way.
//
// This header is pure (no kernel dependencies). memcpy() must already be declared (kernel:
// string.h, host: <cstring>) before this header is included.
//
// Rectangles: x1,y1 inclusive; x2,y2 exclusive.

#include <stdint.h>
#include <stdbool.h>

// Maps a damage rect given in SURFACE coordinates to the part of it that is
// actually on screen, in SCREEN pixel coordinates. The window's client area is
// at (win_x, win_y) with size win_w x win_h and may hang off any screen edge.
// Returns false when nothing visible is damaged.
static inline bool surface_damage_to_screen(
        int32_t win_x, int32_t win_y, int32_t win_w, int32_t win_h,
        int32_t dx1, int32_t dy1, int32_t dx2, int32_t dy2,
        int32_t screen_w, int32_t screen_h,
        int32_t* sx1, int32_t* sy1, int32_t* sx2, int32_t* sy2) {
    // 1. clip the damage to the surface itself
    if (dx1 < 0) dx1 = 0;
    if (dy1 < 0) dy1 = 0;
    if (dx2 > win_w) dx2 = win_w;
    if (dy2 > win_h) dy2 = win_h;
    if (dx1 >= dx2 || dy1 >= dy2) return false;

    // 2. translate to screen space (64-bit: no overflow on hostile values)
    int64_t x1 = (int64_t)win_x + dx1, x2 = (int64_t)win_x + dx2;
    int64_t y1 = (int64_t)win_y + dy1, y2 = (int64_t)win_y + dy2;

    // 3. clip to the screen
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > screen_w) x2 = screen_w;
    if (y2 > screen_h) y2 = screen_h;
    if (x1 >= x2 || y1 >= y2) return false;

    *sx1 = (int32_t)x1; *sy1 = (int32_t)y1;
    *sx2 = (int32_t)x2; *sy2 = (int32_t)y2;
    return true;
}

// The rainbow, rendered as a CLIENT: fills a w x h surface (pitch in bytes).
// The colour of pixel x is wide[(scroll + x) % 720] (wide[j] = lut[j/2]) and does
// not depend on y, so one row is built and replicated. Produces the same pixels as
// wm_draw_rainbow_content() in wm.h.
static inline void rainbow_render_surface(uint32_t* px, uint32_t w, uint32_t h,
                                          uint32_t pitch_bytes, uint32_t scroll,
                                          const uint32_t* wide) {
    if (!px || !wide || w == 0 || h == 0) return;

    uint32_t pos = (uint32_t)((uint64_t)scroll % 720u);
    uint32_t* dst = px;
    uint32_t left = w;
    while (left > 0) {
        uint32_t n = 720u - pos;
        if (n > left) n = left;
        memcpy(dst, &wide[pos], (uint64_t)n * 4);
        dst += n; left -= n; pos = 0;
    }
    for (uint32_t y = 1; y < h; y++)
        memcpy((uint8_t*)px + (uint64_t)y * pitch_bytes, px, (uint64_t)w * 4);
}

// The "Visual Test" window as a client: a 50px checkerboard, drawn ONCE into its surface
// instead of being regenerated in the compositor each time the window is touched.
static inline void checker_render_surface(uint32_t* px, uint32_t w, uint32_t h, uint32_t pitch_bytes) {
    if (!px || w == 0 || h == 0) return;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t* row = (uint32_t*)((uint8_t*)px + (uint64_t)y * pitch_bytes);
        uint32_t ybit = (y / 50) % 2;
        for (uint32_t x = 0; x < w; ) {
            uint32_t span = 50 - (x % 50);
            if (span > w - x) span = w - x;
            uint32_t c = (((x / 50) % 2) == ybit) ? 0xFFEEEEEEu : 0xFFDDDDDDu;
            for (uint32_t i = 0; i < span; i++) row[x + i] = c;
            x += span;
        }
    }
}

#endif
