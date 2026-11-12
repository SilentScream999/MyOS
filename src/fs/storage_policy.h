#ifndef WYZAR_STORAGE_POLICY_H
#define WYZAR_STORAGE_POLICY_H
#include <stdint.h>
static constexpr uint64_t WY_STORAGE_POLICY_LBA = 1;
static bool wy_storage_same(const void* first, const void* second, uint64_t len) {
    auto* a = (const uint8_t*)first; auto* b = (const uint8_t*)second;
    while (len--) if (*a++ != *b++) return false;
    return true;
}
static uint32_t wy_storage_crc(const void* bytes, uint64_t len, uint32_t crc = 0xffffffffu) {
    auto* p = (const uint8_t*)bytes;
    while (len--) { crc ^= *p++; for (unsigned b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u))); }
    return crc;
}
static uint32_t wy_storage_u32(const uint8_t* p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t wy_storage_u64(const uint8_t* p) { return wy_storage_u32(p) | (uint64_t)wy_storage_u32(p + 4) << 32; }
static void wy_storage_p32(uint8_t* p, uint32_t n) { for (unsigned b = 0; b < 4; ++b) p[b] = n >> (b * 8); }
static bool wy_storage_policy_valid(const uint8_t* p) {
    const char magic[16] = "WYZAR-AUTOGROW1";
    for (unsigned i = 0; i < 16; ++i) if (p[i] != (uint8_t)magic[i]) return false;
    return wy_storage_u32(p + 16) == 1 && wy_storage_u32(p + 20) <= 1 &&
           (wy_storage_crc(p, 508) ^ 0xffffffffu) == wy_storage_u32(p + 508);
}
#endif
