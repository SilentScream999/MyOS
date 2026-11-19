#ifndef WYZAR_FAT_TIME_H
#define WYZAR_FAT_TIME_H
#include "fileinfo.h"

static uint32_t fat_encode_time(const file_datetime& t) {
    if (t.year < 1980 || t.year > 2107 || t.month < 1 || t.month > 12 ||
        t.day < 1 || t.day > 31 || t.hour > 23 || t.minute > 59 || t.second > 59)
        return (1u << 21) | (1u << 16); // Earliest valid FAT date if the RTC is unavailable.
    return ((uint32_t)(t.year - 1980) << 25) | ((uint32_t)t.month << 21) |
        ((uint32_t)t.day << 16) | ((uint32_t)t.hour << 11) | ((uint32_t)t.minute << 5) | (t.second / 2);
}
static file_datetime fat_decode_time(uint16_t date, uint16_t time) {
    if (!date) return {};
    return {(uint16_t)(1980 + (date >> 9)), (uint8_t)((date >> 5) & 15),
        (uint8_t)(date & 31), (uint8_t)(time >> 11), (uint8_t)((time >> 5) & 63), (uint8_t)((time & 31) * 2), 0};
}
#endif
