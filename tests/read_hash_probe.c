typedef unsigned long u64;
static unsigned char buffer[65536];
static u64 call(u64 n, u64 a, u64 b, u64 c) {
    u64 result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return result;
}
void _start(void) {
    u64 fd = call(2, (u64)"/share/wall.raw", 0, 0);
    u64 hash = 0xcbf29ce484222325UL;
    int ok = (long)fd >= 0;
    while (ok) {
        u64 count = call(0, fd, (u64)buffer, sizeof(buffer));
        if (count > sizeof(buffer)) { ok = 0; break; }
        if (!count) break;
        for (u64 i = 0; i < count; ++i) hash = (hash ^ buffer[i]) * 0x100000001b3UL;
    }
    if ((long)fd >= 0) call(3, fd, 0, 0);
    char hex[16]; const char* digits = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i) hex[i] = digits[(hash >> (60 - i*4)) & 15];
    fd = call(2, (u64)"/readhash.txt", 0x41, 0);
    if ((long)fd >= 0) {
        ok = ok && call(1, fd, (u64)hex, sizeof(hex)) == sizeof(hex);
        call(3, fd, 0, 0);
    } else ok = 0;
    call(60, !ok, 0, 0);
    for (;;) {}
}
