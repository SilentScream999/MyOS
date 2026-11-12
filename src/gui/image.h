#ifndef WYZAR_IMAGE_H
#define WYZAR_IMAGE_H
#include <stdint.h>

// Owned, premultiplied 0xAARRGGBB pixels, top row first. Release with image_free.
// Premultiplication keeps transparent edges clean when resizing and compositing.
struct Image { uint32_t width, height; uint32_t* pixels; };
void* image_allocate(uint64_t size);
void image_deallocate(void* pointer);
constexpr uint64_t IMAGE_MAX_FILE = 64ull * 1024 * 1024;
constexpr uint64_t IMAGE_MAX_PIXELS = 16ull * 1024 * 1024;
bool image_decode(const uint8_t* bytes, uint64_t size, Image* out,
                  uint64_t max_pixels = IMAGE_MAX_PIXELS);
void image_free(Image* image);
bool image_resize(const Image& source, uint32_t width, uint32_t height, Image* out);
// Source-over onto an opaque desktop target. Alpha is already in source RGB.
uint32_t image_over(uint32_t background, uint32_t foreground);
void image_draw(const Image& image, uint32_t* target, uint32_t width, uint32_t height,
                uint32_t stride, int32_t x, int32_t y,
                int32_t left, int32_t top, int32_t right, int32_t bottom);
#endif
