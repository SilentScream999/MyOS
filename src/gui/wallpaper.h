#ifndef WYZAR_WALLPAPER_H
#define WYZAR_WALLPAPER_H
#include <stdint.h>

// x86-64/SSE2 is the kernel's existing graphics baseline. memcpy into/out of
// these values keeps both image bytes and odd-width rows safe when unaligned.
typedef uint32_t wallpaper_pixels4 __attribute__((vector_size(16)));

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
    // Common case: native-resolution wallpaper. Read BGRA pixels directly and
    // make them opaque; no channel repacking or per-pixel divisions are needed.
    // builtin_memcpy permits unaligned source bytes without aliasing UB.
    uint32_t x_step = sw / w, x_remainder = sw % w;
    const wallpaper_pixels4 opaque = {0xff000000u, 0xff000000u, 0xff000000u, 0xff000000u};
    uint64_t previous_sy = UINT64_MAX;
    for (uint32_t y = 0; y < h; ++y) {
        uint64_t sy = (uint64_t)y * sh / h;
        uint32_t* row = out + (uint64_t)y * w;
        if (sy == previous_sy) {
            // Upscaling often repeats a source row; its converted pixels exist.
            const uint32_t* previous = row - w;
            uint32_t x = 0;
            for (; w - x >= 4; x += 4) {
                wallpaper_pixels4 pixels;
                __builtin_memcpy(&pixels, previous + x, 16);
                __builtin_memcpy(row + x, &pixels, 16);
            }
            for (; x < w; ++x) row[x] = previous[x];
        } else if (sw == w) {
            const uint8_t* source = data + sy * sw * 4;
            uint32_t x = 0;
            for (; w - x >= 4; x += 4) {
                wallpaper_pixels4 pixels;
                __builtin_memcpy(&pixels, source + (uint64_t)x * 4, 16);
                pixels |= opaque;
                __builtin_memcpy(row + x, &pixels, 16);
            }
            for (; x < w; ++x) {
                uint32_t pixel;
                __builtin_memcpy(&pixel, source + (uint64_t)x * 4, 4);
                row[x] = pixel | 0xff000000u;
            }
        } else {
            // Exact floor(x * sw / w), advanced incrementally. Keep the error
            // in 64 bits so dimensions near UINT32_MAX cannot wrap it.
            uint64_t sx = 0, error = 0;
            const uint8_t* source = data + sy * sw * 4;
            for (uint32_t x = 0; x < w; ++x) {
                uint32_t pixel;
                __builtin_memcpy(&pixel, source + sx * 4, 4);
                row[x] = pixel | 0xff000000u;
                sx += x_step; error += x_remainder;
                if (error >= w) { ++sx; error -= w; }
            }
        }
        previous_sy = sy;
    }
    return true;
}
#endif
