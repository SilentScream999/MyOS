#ifndef damage_h
#define damage_h

// ─── damage.h ───────────────────────────────────────────────────────────────
// Damage LIST: up to DAMAGE_MAX separate dirty rectangles instead of one bounding box.
//
// With a single bounding box, a 1-per-second HUD refresh in the top-right corner plus an animation in the
// middle of the screen forced the compositor to recompose and flip ONE huge box covering both (and the HUD
// line also went stale because its strip was too narrow). With a list, each region is composed and flipped
// on its own.
//
// Rectangles use the compositor's existing convention: x1,y1,x2,y2 are MIN/MAX values (x2 inclusive; see
// wm_compose_dirty for the y quirk), so a rect handed to wm_compose_dirty(x1,x2,y1,y2) behaves exactly as
// before. Pure header (no kernel dependencies).
// Callers must disable interrupts around damage_add()/damage_take() (see wm_dirty_add in framebufferstuff.h).

#include <stdint.h>
#include <stdbool.h>

#define DAMAGE_MAX 8

struct DmgRect { uint32_t x1, y1, x2, y2; };
struct DamageList { DmgRect r[DAMAGE_MAX]; uint32_t n; };

static inline void damage_clear(DamageList* d) { d->n = 0; }

// Overlapping or adjacent (gap <= 1 pixel). 64-bit math: no overflow on extreme inputs.
static inline bool dmg_touch(const DmgRect& a, const DmgRect& b) {
    return (uint64_t)a.x1 <= (uint64_t)b.x2 + 1 && (uint64_t)b.x1 <= (uint64_t)a.x2 + 1 &&
           (uint64_t)a.y1 <= (uint64_t)b.y2 + 1 && (uint64_t)b.y1 <= (uint64_t)a.y2 + 1;
}
static inline DmgRect dmg_union(const DmgRect& a, const DmgRect& b) {
    DmgRect u;
    u.x1 = a.x1 < b.x1 ? a.x1 : b.x1;  u.y1 = a.y1 < b.y1 ? a.y1 : b.y1;
    u.x2 = a.x2 > b.x2 ? a.x2 : b.x2;  u.y2 = a.y2 > b.y2 ? a.y2 : b.y2;
    return u;
}
static inline uint64_t dmg_area(const DmgRect& a) {
    return ((uint64_t)a.x2 - a.x1 + 1) * ((uint64_t)a.y2 - a.y1 + 1);
}

// Add a dirty rect. Guarantees: (1) every pixel of every rect ever added stays covered by some list rect;
// (2) n <= DAMAGE_MAX; (3) no two list rects touch (touching rects are merged). When the list is full the
// new rect is merged into whichever existing rect it wastes the least area with.
static inline void damage_add(DamageList* d, uint32_t min_x, uint32_t max_x, uint32_t min_y, uint32_t max_y) {
    if (min_x > max_x || min_y > max_y) return;      // empty / inverted: nothing damaged
    DmgRect cur = { min_x, min_y, max_x, max_y };
    for (;;) {
        // absorb everything that touches cur (cur grows, so repeat until stable)
        bool merged;
        do {
            merged = false;
            for (uint32_t i = 0; i < d->n; ) {
                if (dmg_touch(d->r[i], cur)) {
                    cur = dmg_union(cur, d->r[i]);
                    d->r[i] = d->r[--d->n];
                    merged = true;
                } else i++;
            }
        } while (merged);

        if (d->n < DAMAGE_MAX) { d->r[d->n++] = cur; return; }

        // Full: merge cur with the existing rect that makes the union waste the least area.
        uint32_t best = 0; uint64_t best_waste = ~0ULL;
        for (uint32_t i = 0; i < d->n; i++) {
            uint64_t ua = dmg_area(dmg_union(cur, d->r[i]));
            uint64_t sum = dmg_area(cur) + dmg_area(d->r[i]);
            uint64_t waste = ua > sum ? ua - sum : 0;
            if (waste < best_waste) { best_waste = waste; best = i; }
        }
        cur = dmg_union(cur, d->r[best]);
        d->r[best] = d->r[--d->n];
        // loop: the grown rect may now touch others
    }
}

// Same, but clipped to the screen first. A rect entirely off-screen is dropped (wm_compose_dirty would
// otherwise turn "min_x >= width" into "min_x = 0" and recompose the whole row span).
static inline void damage_add_clipped(DamageList* d, uint32_t screen_w, uint32_t screen_h,
                                      uint32_t min_x, uint32_t max_x, uint32_t min_y, uint32_t max_y) {
    if (screen_w == 0 || screen_h == 0) return;
    if (min_x >= screen_w || min_y >= screen_h) return;
    if (max_x >= screen_w) max_x = screen_w - 1;
    if (max_y >= screen_h) max_y = screen_h - 1;
    damage_add(d, min_x, max_x, min_y, max_y);
}

#endif
