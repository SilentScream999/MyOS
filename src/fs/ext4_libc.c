// Private libc subset for lwext4; symbols never replace the kernel's string API.
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *a = d; const unsigned char *b = s;
    for (size_t i = 0; i < n; ++i) a[i] = b[i];
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *a = d; const unsigned char *b = s;
    if ((uintptr_t)a < (uintptr_t)b) return memcpy(d, s, n);
    while (n) { --n; a[n] = b[n]; }
    return d;
}
void *memset(void *d, int c, size_t n) {
    unsigned char *a = d; for (size_t i = 0; i < n; ++i) a[i] = c; return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; ++i) if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; } return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) break;
    }
    return 0;
}
char *strcpy(char *d, const char *s) { char *p = d; while ((*p++ = *s++)) {} return d; }
void *calloc(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) return NULL;
    void *p = malloc(n * size); if (p) memset(p, 0, n * size); return p;
}
// In-place heapsort: bounded stack and no allocations during directory indexing.
static void swap(unsigned char *a, unsigned char *b, size_t size) {
    while (size--) { unsigned char c = *a; *a++ = *b; *b++ = c; }
}
static void sift(unsigned char *a, size_t root, size_t count, size_t size,
                 int (*cmp)(const void *, const void *)) {
    while (root < count / 2) {
        size_t child = root * 2 + 1;
        if (child + 1 < count && cmp(a + child * size, a + (child + 1) * size) < 0) ++child;
        if (cmp(a + root * size, a + child * size) >= 0) break;
        swap(a + root * size, a + child * size, size); root = child;
    }
}
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    if (!size || n < 2 || n > SIZE_MAX / size) return;
    unsigned char *a = base;
    for (size_t i = n / 2; i; ) sift(a, --i, n, size, cmp);
    for (size_t end = n; end > 1; ) { --end; swap(a, a + end * size, size); sift(a, 0, end, size, cmp); }
}

// Clang can emit these for aggregate copies even with -fno-builtin.
#undef memcpy
#undef memset
#undef memmove
void *memcpy(void *d, const void *s, size_t n) { return wyzar_ext4_memcpy(d, s, n); }
void *memset(void *d, int c, size_t n) { return wyzar_ext4_memset(d, c, n); }
void *memmove(void *d, const void *s, size_t n) { return wyzar_ext4_memmove(d, s, n); }
