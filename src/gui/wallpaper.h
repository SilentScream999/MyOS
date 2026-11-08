#ifndef WYZAR_WALLPAPER_H
#define WYZAR_WALLPAPER_H
#include <stdint.h>

static uint32_t wallpaper_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Validate completely before touching the caller's destination buffer.
static bool wallpaper_decode(const uint8_t* data, uint64_t size,
                             uint32_t w, uint32_t h, uint32_t* out) {
    if (!data || !out || !w || !h) return false;
    uint32_t sw = w, sh = h;
    if (size >= 16 && data[0] == 'W' && data[1] == 'R' && data[2] == 'A' && data[3] == 'W') {
        sw = wallpaper_u32(data + 4); sh = wallpaper_u32(data + 8);
        if (!sw || !sh || wallpaper_u32(data + 12) != 0 ||
            (uint64_t)sw * sh > (size - 16) / 4) return false;
        data += 16;
    } else if ((uint64_t)w * h > UINT64_MAX / 4 || size != (uint64_t)w * h * 4) {
        return false;
    }
    for (uint32_t y = 0; y < h; ++y) {
        uint64_t sy = (uint64_t)y * sh / h;
        for (uint32_t x = 0; x < w; ++x) {
            uint64_t sx = (uint64_t)x * sw / w;
            const uint8_t* p = data + (sy * sw + sx) * 4;
            out[(uint64_t)y * w + x] = 0xff000000u | ((uint32_t)p[2] << 16) |
                                      ((uint32_t)p[1] << 8) | p[0];
        }
    }
    return true;
}
#endif
