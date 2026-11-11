#ifndef WYZAR_EXT4_STRING_H
#define WYZAR_EXT4_STRING_H
#include <stddef.h>
#define memcpy wyzar_ext4_memcpy
#define memmove wyzar_ext4_memmove
#define memset wyzar_ext4_memset
#define memcmp wyzar_ext4_memcmp
#define strlen wyzar_ext4_strlen
#define strcmp wyzar_ext4_strcmp
#define strncmp wyzar_ext4_strncmp
#define strcpy wyzar_ext4_strcpy
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
char *strcpy(char *, const char *);
#endif
