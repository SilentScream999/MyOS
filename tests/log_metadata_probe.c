typedef unsigned long u64;
#include "../src/fs/fileinfo.h"
static u64 call(u64 n, u64 a, u64 b, u64 c, u64 d, u64 e) {
    register u64 r10 __asm__("r10") = d;
    register u64 r8 __asm__("r8") = e;
    u64 result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8) : "rcx", "r11", "memory");
    return result;
}
static int same(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
static int check(const char* path, const char* directory, const char* entry) {
    file_info info;
    if (call(4, (u64)path, (u64)&info, 0, 0, 0) || !info.size || info.size > 65535 || info.type != 1) return 0;
    char data[65536];
    u64 fd = call(2, (u64)path, 0, 0, 0, 0);
    if ((long)fd < 0) return 0;
    u64 read = call(0, fd, (u64)data, sizeof(data), 0, 0);
    call(3, fd, 0, 0, 0, 0);
    if (read != info.size) return 0;
    for (u64 i = 0; i < 512; ++i) {
        char name[256]; u64 size, type;
        if (call(40, (u64)directory, i, (u64)name, (u64)&size, (u64)&type) != 1) return 0;
        if (same(name, entry)) return size == info.size && type == 1;
    }
    return 0;
}
void _start(void) {
    int ok = check("/bootlog.txt", "/", "BOOTLOG.TXT") && check("/run/boot.log", "/run", "boot.log");
    u64 fd = call(2, (u64)"/logcheck.txt", 0x41, 0, 0, 0);
    if ((long)fd >= 0) {
        call(1, fd, (u64)(ok ? "PASS" : "FAIL"), 4, 0, 0);
        call(3, fd, 0, 0, 0, 0);
    }
    call(60, !ok, 0, 0, 0, 0);
    for (;;) {}
}
