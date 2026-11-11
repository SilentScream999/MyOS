#include <cassert>
#include <cstring>
#include <vector>
#include "../src/gui/wallpaper.h"
int main() {
    uint32_t out[4] = {1, 2, 3, 4};
    uint8_t image[20] = {'W','R','A','W',1,0,0,0,1,0,0,0,0,0,0,0,3,2,1,0};
    assert(wallpaper_decode(image, sizeof(image), 2, 2, out));
    for (auto pixel : out) assert(pixel == 0xff010203);
    uint32_t before[4]; memcpy(before, out, sizeof(out));
    assert(!wallpaper_decode(image, 19, 2, 2, out));
    image[12] = 1;
    assert(!wallpaper_decode(image, sizeof(image), 2, 2, out));
    image[12] = 0;
    memset(image + 4, 255, 8); // dimension multiplication must not wrap
    assert(!wallpaper_decode(image, sizeof(image), 2, 2, out));
    assert(memcmp(before, out, sizeof(out)) == 0);
    uint8_t legacy[4] = {6,5,4,0};
    assert(wallpaper_decode(legacy, 4, 1, 1, out));
    assert(out[0] == 0xff040506);
    assert(!wallpaper_decode(legacy, 3, 1, 1, out));

    // Compare all fast/scaling paths against the original nearest-neighbor
    // definition: native size, odd ratios, downscaling and repeated rows.
    for (uint32_t sw : {1u, 2u, 3u, 7u, 19u}) {
        for (uint32_t sh : {1u, 2u, 5u, 11u}) {
            // Deliberately unaligned input: packed reads must remain defined.
            std::vector<uint8_t> storage(1 + 16 + (uint64_t)sw * sh * 4);
            uint8_t* source = storage.data() + 1;
            memcpy(source, "WRAW", 4);
            auto put32 = [](uint8_t* p, uint32_t n) {
                for (unsigned b = 0; b < 4; ++b) p[b] = (uint8_t)(n >> (8 * b));
            };
            put32(source + 4, sw); put32(source + 8, sh); put32(source + 12, 0);
            for (uint64_t i = 0; i < (uint64_t)sw * sh * 4; ++i) source[16 + i] = (uint8_t)(i * 37 + 11);
            for (uint32_t w : {1u, 2u, 3u, 7u, 13u, 31u}) {
                for (uint32_t h : {1u, 2u, 5u, 9u, 17u}) {
                    std::vector<uint32_t> pixels((uint64_t)w * h + 2, 0x12345678);
                    assert(wallpaper_decode(source, storage.size() - 1, w, h, pixels.data() + 1));
                    assert(pixels.front() == 0x12345678 && pixels.back() == 0x12345678);
                    for (uint32_t y = 0; y < h; ++y) {
                        for (uint32_t x = 0; x < w; ++x) {
                            uint64_t sx = (uint64_t)x * sw / w, sy = (uint64_t)y * sh / h;
                            const uint8_t* p = source + 16 + (sy * sw + sx) * 4;
                            uint32_t expected = 0xff000000u | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
                            assert(pixels[1 + (uint64_t)y * w + x] == expected);
                        }
                    }
                }
            }
        }
    }
}
