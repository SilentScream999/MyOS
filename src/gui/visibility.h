#ifndef WYZAR_VISIBILITY_H
#define WYZAR_VISIBILITY_H

#include <stdint.h>

// Screen rectangles have exclusive right/bottom edges. Fixed storage keeps
// animation visibility queries allocation-free, including fragmented overlaps.
struct VisibleRect { int32_t x1, y1, x2, y2; };
struct VisibleRegion {
    static constexpr unsigned capacity = 64;
    VisibleRect rects[capacity];
    unsigned count;
};

static inline VisibleRect visibility_intersect(VisibleRect a, VisibleRect b) {
    return {a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1,
            a.x2 < b.x2 ? a.x2 : b.x2, a.y2 < b.y2 ? a.y2 : b.y2};
}
static inline bool visibility_valid(VisibleRect r) {
    return r.x1 < r.x2 && r.y1 < r.y2;
}

// On overflow leave the input untouched. Callers may conservatively retain
// covered pixels, but must never lose exposed pixels.
static inline bool visibility_subtract(VisibleRegion* region, VisibleRect cover) {
    VisibleRegion next{};
    for (unsigned i = 0; i < region->count; ++i) {
        VisibleRect r = region->rects[i], cut = visibility_intersect(r, cover);
        VisibleRect pieces[4]; unsigned n = 0;
        if (!visibility_valid(cut)) pieces[n++] = r;
        else {
            if (r.y1 < cut.y1) pieces[n++] = {r.x1,r.y1,r.x2,cut.y1};
            if (cut.y2 < r.y2) pieces[n++] = {r.x1,cut.y2,r.x2,r.y2};
            if (r.x1 < cut.x1) pieces[n++] = {r.x1,cut.y1,cut.x1,cut.y2};
            if (cut.x2 < r.x2) pieces[n++] = {cut.x2,cut.y1,r.x2,cut.y2};
        }
        if (next.count + n > VisibleRegion::capacity) return false;
        for (unsigned p = 0; p < n; ++p) next.rects[next.count++] = pieces[p];
    }
    region->count = next.count;
    for (unsigned i = 0; i < next.count; ++i) region->rects[i] = next.rects[i];
    return true;
}

// Clamp in 64 bits before converting back to screen coordinates.
static inline VisibleRect visibility_clip(int32_t x, int32_t y, int32_t w, int32_t h,
                                          int32_t top, int32_t screen_w, int32_t screen_h) {
    int64_t left=x, upper=(int64_t)y-top, right=(int64_t)x+w, bottom=(int64_t)y+h;
    if (left < 0) left=0;
    if (upper < 0) upper=0;
    if (right > screen_w) right=screen_w;
    if (bottom > screen_h) bottom=screen_h;
    // An empty intersection need not preserve its out-of-range coordinates.
    if (left >= right || upper >= bottom) return {0,0,0,0};
    return {(int32_t)left,(int32_t)upper,(int32_t)right,(int32_t)bottom};
}

#endif
