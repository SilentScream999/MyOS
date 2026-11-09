#ifndef WYZAR_FILEINFO_H
#define WYZAR_FILEINFO_H
#include <stdint.h>
// Shared syscall ABI. Civil local time, matching the desktop RTC clock.
// year == 0 means unknown. FAT stores modification seconds in two-second steps.
typedef struct file_datetime {
    uint16_t year;
    uint8_t month, day, hour, minute, second, reserved;
} file_datetime;
typedef struct file_info {
    uint64_t size;
    uint64_t type; // 1=file, 2=directory, 3=device
    file_datetime created;
    file_datetime modified;
} file_info;

typedef struct fs_info {
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    char     fs_type[16];
} fs_info;
#endif
