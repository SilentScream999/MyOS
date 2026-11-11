#ifndef WYZAR_EXT4_STDLIB_H
#define WYZAR_EXT4_STDLIB_H
#include <stddef.h>
#define malloc wyzar_ext4_malloc
#define calloc wyzar_ext4_calloc
#define realloc wyzar_ext4_realloc
#define free wyzar_ext4_free
#define qsort wyzar_ext4_qsort
void *malloc(size_t);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void free(void *);
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
#endif
