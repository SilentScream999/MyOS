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
#endif
