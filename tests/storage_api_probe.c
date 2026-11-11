typedef unsigned long u64;
#include "../src/fs/fileinfo.h"
static long call(u64 nr, u64 a, u64 b, u64 c) {
    long result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(nr), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return result;
}
static unsigned stage;
static int check(const char* path, const char* renamed, unsigned base) {
    stage = base;
    if (call(138, (u64)path, (u64)"old-long-content", 16) ||
        call(138, (u64)path, (u64)"ok", 2)) return 0;
    long fd = call(2, (u64)path, 2, 0);
    if (fd < 0) return 0;
    char bytes[8192];
    stage = base + 1;
    int ok = call(0, fd, (u64)bytes, sizeof(bytes)) == 2 && bytes[0] == 'o' && bytes[1] == 'k';
    if (ok) { stage = base + 2; ok = call(8, fd, 0, 2) == 2; }
    if (ok) { stage = base + 3; ok = call(1, fd, (u64)"!", 1) == 1; }
    if (ok) { stage = base + 4; ok = call(74, fd, 0, 0) == 0; }
    if (ok) { stage = base + 5; ok = call(77, fd, 1, 0) == 0; }
    if (ok) { stage = base + 6; ok = call(77, fd, 5121, 0) == 0; }
    if (ok) { stage = base + 7; ok = call(8, fd, 0, 0) == 0; }
    if (ok) { stage = base + 8; ok = call(0, fd, (u64)bytes, sizeof(bytes)) == 5121 && bytes[0] == 'o'; }
    for (unsigned i = 1; ok && i < 5121; ++i) if (bytes[i]) ok = 0;
    call(3, fd, 0, 0);
    if (ok) stage = base + 9;
    return ok && call(82, (u64)path, (u64)renamed, 0) == 0 && call(2, (u64)path, 0, 0) == -1;
}
void _start(void) {
#ifdef STORAGE_API_WORKER
    int even = (call(39, 0, 0, 0) & 1) == 0;
    char content[16385];
    for (unsigned i = 0; i < sizeof(content); ++i) content[i] = even ? 'E' : 'O';
    const char* path = even ? "/home/concurrent-even.bin" : "/home/concurrent-odd.bin";
    // Leave a descriptor open to exercise scheduler close/reap while another
    // task may own the filesystem mutex.
    long fd = call(2, (u64)(even ? "/home/open-even.txt" : "/home/open-odd.txt"), 0x41, 0);
    int ok = fd >= 0;
    for (unsigned i = 0; ok && i < 5; ++i) ok = call(138, (u64)path, (u64)content, sizeof(content)) == 0;
    call(60, !ok, 0, 0);
#else
    fs_info info;
    stage = 1;
    int ok = call(137, (u64)"/", (u64)&info, 0) == 0 && info.total_bytes >= 1024UL * 1024 * 1024;
    ok = ok && check("/home/api.txt", "/home/api-renamed.txt", 10);
    ok = ok && check("/tmp/api.txt", "/tmp/api-renamed.txt", 40);
    if (ok) {
        stage = 70;
        long first = call(59, (u64)"/bin/apiworker.elf", 0, 0);
        long second = call(59, (u64)"/bin/apiworker.elf", 0, 0);
        u64 code_a = 99, code_b = 99;
        ok = first > 0 && second > 0 && call(61, first, (u64)&code_a, 0) == first &&
             call(61, second, (u64)&code_b, 0) == second && !code_a && !code_b;
    }
    char failed[] = "FAIL-00";
    failed[5] = '0' + stage / 10; failed[6] = '0' + stage % 10;
    call(138, (u64)"/home/api-result.txt", (u64)(ok ? "PASS" : failed), ok ? 4 : 7);
    call(60, !ok, 0, 0);
#endif
    for (;;) {}
}
