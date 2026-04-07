#ifndef STRING_H
#define STRING_H

#include <stdint.h>
#include <stddef.h>

static inline uint64_t strlen(const char* str) {
	uint64_t len = 0;
	while (str[len]) len++;
	return len;
}

static inline void* memcpy(void* dest, const void* src, uint64_t n) {
    uint64_t n64 = n / 8;
    uint64_t n8  = n % 8;
    __asm__ volatile (
        "rep movsq\n"
        "mov %3, %%rcx\n"
        "rep movsb"
        : "+D"(dest), "+S"(src)
        : "c"(n64), "r"(n8)
        : "memory"
    );
    return dest;
}

static inline void* memset(void* dst, int val, size_t n) {
    uint64_t n64 = n / 8;
    uint64_t n8  = n % 8;
    uint64_t v64 = (uint8_t)val;
    v64 |= (v64 << 8);
    v64 |= (v64 << 16);
    v64 |= (v64 << 32);

    void* d = dst;
    __asm__ volatile (
        "rep stosq\n"
        "mov %2, %%rcx\n"
        "rep stosb"
        : "+D"(d)
        : "c"(n64), "r"(n8), "a"(v64)
        : "memory"
    );
    return dst;
}

static inline void memset_32(void* dest, uint32_t val, uint64_t count) {
    uint32_t* d = (uint32_t*)dest;
    uint64_t n8 = count / 2; // two 32-bit vals per stosq
    uint64_t n_rem = count % 2;
    if (n8 > 0) {
        uint64_t v64 = val;
        v64 |= (v64 << 32);
        __asm__ volatile ("rep stosq" : "+D"(d), "+c"(n8) : "a"(v64) : "memory");
    }
    if (n_rem) *d++ = val;
}

static inline void* memmove(void* dst, const void* src, size_t n) {
    if (dst < src) return memcpy(dst, src, n);
    uint8_t* d = (uint8_t*)dst + n;
    const uint8_t* s = (const uint8_t*)src + n;
    while (n--) *--d = *--s;
    return dst;
}

static inline char* strcpy(char* dest, const char* src) {
	char* d = dest;
	while ((*d++ = *src++));
	return dest;
}

static inline void vram_fence() {
    __asm__ volatile ("sfence" ::: "memory");
}

// SSE-accelerated VRAM transfer (Headless - NO SFENCE)
// To be used in loops; call vram_fence() once at the end.
static inline void* memcpy_vram_sse_headless(void* dest, const void* src, uint64_t n) {
    if (n == 0) return dest;
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    while (n > 0 && ((uintptr_t)d & 15) != 0) {
        *d++ = *s++; n--;
    }
    uint64_t n64 = n / 64;
    uint64_t n_rem_total = n % 64;
    if (n64 > 0) {
        __asm__ volatile (
            "1:\n"
            "movups 0(%1),  %%xmm0\n"
            "movups 16(%1), %%xmm1\n"
            "movups 32(%1), %%xmm2\n"
            "movups 48(%1), %%xmm3\n"
            "movntdq %%xmm0, 0(%0)\n"
            "movntdq %%xmm1, 16(%0)\n"
            "movntdq %%xmm2, 32(%0)\n"
            "movntdq %%xmm3, 48(%0)\n"
            "add $64, %0\n"
            "add $64, %1\n"
            "dec %2\n"
            "jnz 1b\n"
            : "+r"(d), "+r"(s), "+r"(n64) :: "xmm0", "xmm1", "xmm2", "xmm3", "memory"
        );
    }
    uint64_t n16 = n_rem_total / 16;
    uint64_t n_rem = n_rem_total % 16;
    if (n16 > 0) {
        __asm__ volatile (
            "2:\n"
            "movups (%1), %%xmm0\n"
            "movntdq %%xmm0, (%0)\n"
            "add $16, %0\n"
            "add $16, %1\n"
            "dec %2\n"
            "jnz 2b\n"
            : "+r"(d), "+r"(s), "+r"(n16) :: "xmm0", "memory"
        );
    }
    if (n_rem > 0) {
        while (n_rem--) *d++ = *s++;
    }
    return dest;
}

static inline void* memcpy_vram_sse(void* dest, const void* src, uint64_t n) {
    void* r = memcpy_vram_sse_headless(dest, src, n);
    vram_fence();
    return r;
}

#endif
