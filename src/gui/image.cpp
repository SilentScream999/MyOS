#include "image.h"
#include <stddef.h>
extern "C" {
#include <stdlib.h>
#include <string.h>
}

// Supplied by the kernel heap, or by the host test harness.
extern void* image_allocate(uint64_t);
extern void image_deallocate(void*);
struct alignas(16) ImageAllocation {
    uint64_t size; bool charged;
    ImageAllocation *previous, *next;
};
static uint64_t decode_used, decode_budget;
static bool decoding;
static ImageAllocation* decode_head;
static void decoder_uncharge(ImageAllocation* allocation) {
    if (!allocation->charged) return;
    if (allocation->previous) allocation->previous->next=allocation->next;
    else decode_head=allocation->next;
    if (allocation->next) allocation->next->previous=allocation->previous;
    decode_used-=allocation->size; allocation->charged=false;
    allocation->previous=allocation->next=nullptr;
}
static void* decoder_alloc(size_t size) {
    if (!size || size > decode_budget - decode_used) return nullptr;
    auto* allocation = (ImageAllocation*)image_allocate(sizeof(ImageAllocation) + size);
    if (!allocation) return nullptr;
    *allocation = {size, true, nullptr, decode_head};
    if (decode_head) decode_head->previous=allocation;
    decode_head=allocation; decode_used += size;
    return allocation + 1;
}
static void decoder_free(void* pointer) {
    if (!pointer) return;
    auto* allocation = (ImageAllocation*)pointer - 1;
    decoder_uncharge(allocation);
    image_deallocate(allocation);
}
static void* decoder_realloc(void* pointer, size_t size) {
    if (!pointer) return decoder_alloc(size);
    if (!size) { decoder_free(pointer); return nullptr; }
    auto* old = (ImageAllocation*)pointer - 1;
    void* replacement = decoder_alloc(size);
    if (!replacement) return nullptr;
    memcpy(replacement, pointer, old->size < size ? old->size : size);
    decoder_free(pointer); return replacement;
}
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_FAILURE_STRINGS
#define STBI_MAX_DIMENSIONS 8192
#define STBI_ASSERT(x) ((void)0)
#define STBI_MALLOC(n) decoder_alloc(n)
#define STBI_REALLOC(p,n) decoder_realloc(p,n)
#define STBI_FREE(p) decoder_free(p)
#define STB_IMAGE_IMPLEMENTATION
#include "../../third_party/stb/stb_image.h"

static uint32_t image_be32(const uint8_t* p) {
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
// stb does not validate PNG CRCs. Validate framing and every CRC before decoding,
// including IEND, so truncated/corrupt files never become published resources.
static bool png_valid(const uint8_t* bytes, uint64_t size) {
    uint64_t offset = 8; bool header = false, data = false, data_ended = false;
    while (offset <= size && size - offset >= 12) {
        uint32_t count = image_be32(bytes + offset);
        if (count > size - offset - 12) return false;
        const uint8_t* type = bytes + offset + 4;
        uint32_t crc = 0xffffffffu;
        for (uint64_t i = 0; i < (uint64_t)count + 4; ++i) {
            crc ^= type[i];
            for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
        }
        if (~crc != image_be32(type + 4 + count)) return false;
        uint32_t tag = image_be32(type);
        if (!header) { if (tag != 0x49484452 || count != 13) return false; header = true; }
        else if (tag == 0x49484452) return false;
        if (tag == 0x49444154) { if (data_ended) return false; data = true; }
        else if (data) data_ended = true;
        offset += (uint64_t)count + 12;
        if (tag == 0x49454e44) return !count && data && offset == size;
    }
    return false;
}
bool image_decode(const uint8_t* bytes, uint64_t size, Image* out, uint64_t max_pixels) {
    if (!bytes || !out || out->pixels || !size || size > IMAGE_MAX_FILE || !max_pixels) return false;
    if (max_pixels > IMAGE_MAX_PIXELS) max_pixels = IMAGE_MAX_PIXELS;
    bool png = size >= 8 && !memcmp(bytes, "\x89PNG\r\n\x1a\n", 8);
    bool jpeg = size >= 4 && bytes[0] == 255 && bytes[1] == 216 &&
                bytes[size-2] == 255 && bytes[size-1] == 217;
    if ((!png && !jpeg) || (png && !png_valid(bytes, size))) return false;
    // Decoder internals use a bounded scratch budget. A concurrent request fails
    // cleanly; callers can retry, rather than spinning with the resource worker.
    if (__atomic_test_and_set(&decoding, __ATOMIC_ACQUIRE)) return false;
    decode_used = 0;
    decode_budget = max_pixels * 12 + 1024 * 1024;
    int w = 0, h = 0, channels = 0;
    uint8_t* rgba = nullptr;
    if (stbi_info_from_memory(bytes, (int)size, &w, &h, &channels) && w > 0 && h > 0 &&
        (uint64_t)w * h <= max_pixels)
        rgba = stbi_load_from_memory(bytes, (int)size, &w, &h, &channels, 4);
    if (rgba) {
        auto* allocation = (ImageAllocation*)rgba - 1;
        decoder_uncharge(allocation);
        uint32_t* pixels = (uint32_t*)rgba;
        for (uint64_t i = 0; i < (uint64_t)w * h; ++i) {
            uint32_t r = rgba[i*4], g = rgba[i*4+1], b = rgba[i*4+2], a = rgba[i*4+3];
            pixels[i] = a<<24 | ((r*a+127)/255)<<16 | ((g*a+127)/255)<<8 | (b*a+127)/255;
        }
        *out = {(uint32_t)w, (uint32_t)h, pixels};
    }
    // Contain any upstream error-path scratch leak, including failed 16->8-bit
    // conversion. Only the published output allocation survives this request.
    while (decode_head) decoder_free(decode_head+1);
    __atomic_clear(&decoding, __ATOMIC_RELEASE);
    return rgba != nullptr;
}
void image_free(Image* image) {
    if (image) { decoder_free(image->pixels); *image = {}; }
}
uint32_t image_over(uint32_t background, uint32_t foreground) {
    uint32_t alpha = foreground >> 24;
    if (!alpha) return background;
    if (alpha == 255) return foreground;
    uint32_t result = 0xff000000;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        uint32_t channel = ((foreground >> shift) & 255) +
                           (((background >> shift) & 255) * (255-alpha) + 127) / 255;
        result |= (channel > 255 ? 255 : channel) << shift;
    }
    return result;
}
bool image_resize(const Image& source, uint32_t width, uint32_t height, Image* out) {
    if (!source.pixels || !source.width || !source.height || !out || out->pixels ||
        !width || !height || (uint64_t)width * height > IMAGE_MAX_PIXELS) return false;
    uint64_t size = (uint64_t)width * height * 4;
    auto* allocation = (ImageAllocation*)image_allocate(sizeof(ImageAllocation) + size);
    if (!allocation) return false;
    *allocation = {size, false};
    auto* pixels = (uint32_t*)(allocation + 1);
    // Bilinear sampling at pixel centers, in premultiplied space.
    for (uint32_t y = 0; y < height; ++y) {
        int64_t fy = (int64_t)((2ull*y+1)*source.height*65536/(2ull*height)) - 32768;
        if (fy < 0) fy = 0;
        uint32_t y0 = fy >> 16, y1 = y0+1 < source.height ? y0+1 : y0, ay = fy & 65535;
        for (uint32_t x = 0; x < width; ++x) {
            int64_t fx = (int64_t)((2ull*x+1)*source.width*65536/(2ull*width)) - 32768;
            if (fx < 0) fx = 0;
            uint32_t x0 = fx >> 16, x1 = x0+1 < source.width ? x0+1 : x0, ax = fx & 65535;
            uint32_t result = 0;
            for (unsigned shift = 0; shift < 32; shift += 8) {
                uint64_t top = ((source.pixels[(uint64_t)y0*source.width+x0]>>shift)&255)*(65536ull-ax) +
                               ((source.pixels[(uint64_t)y0*source.width+x1]>>shift)&255)*(uint64_t)ax;
                uint64_t bottom = ((source.pixels[(uint64_t)y1*source.width+x0]>>shift)&255)*(65536ull-ax) +
                                  ((source.pixels[(uint64_t)y1*source.width+x1]>>shift)&255)*(uint64_t)ax;
                result |= (uint32_t)((top*(65536-ay)+bottom*ay+(1ull<<31))>>32)<<shift;
            }
            pixels[(uint64_t)y*width+x] = result;
        }
    }
    *out = {width, height, pixels}; return true;
}
void image_draw(const Image& image, uint32_t* target, uint32_t width, uint32_t height,
                uint32_t stride, int32_t x, int32_t y,
                int32_t left, int32_t top, int32_t right, int32_t bottom) {
    if (!image.pixels || !target || stride < width) return;
    if (left < 0) left = 0; if (top < 0) top = 0;
    if (right > (int64_t)width) right = width; if (bottom > (int64_t)height) bottom = height;
    for (int64_t py = top > y ? top : y; py < bottom && py < (int64_t)y+image.height; ++py)
        for (int64_t px = left > x ? left : x; px < right && px < (int64_t)x+image.width; ++px) {
            uint32_t* pixel = target + (uint64_t)py*stride+px;
            *pixel = image_over(*pixel, image.pixels[(py-y)*image.width+(px-x)]);
        }
}
