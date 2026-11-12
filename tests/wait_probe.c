typedef unsigned long u64;
typedef long i64;
static u64 call(u64 nr, u64 a, u64 b) {
    u64 result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(nr), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return result;
}
static void output(const char* s) {
    u64 n = 0; while (s[n]) ++n;
    u64 result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(1UL), "D"(1UL), "S"(s), "d"(n) : "rcx", "r11", "memory");
}
void _start(void) {
#ifdef EXIT_ONLY
    call(60, 7, 0);
#elif defined(READ_ONLY)
#ifdef ENABLE_TEST_ECHO
    call(18, 1, 0);
#endif
    output("[input-probe] running, waiting for a line\n");
    char c = 0;
    while (c != '\n') {
        u64 result;
        __asm__ volatile("syscall" : "=a"(result) : "a"(0UL), "D"(0UL), "S"(&c), "d"(1UL) : "rcx", "r11", "memory");
    }
    output("[input-probe] finished\n");
    call(60, 0, 0);
#elif defined(IGNORE_INPUT)
    output("[ignore-probe] running\n");
    call(35, 3000, 0);
    output("[ignore-probe] finished\n");
    call(60, 0, 0);
#elif defined(BUSY_LOOP)
    call(2, (u64)"/disk/lock.txt", 0x41);
    output("[busy-probe] running\n");
    for (;;) __asm__ volatile("pause");
#else
    u64 status = 99;
    i64 pid = (i64)call(59, (u64)"/disk/exit.elf", 0);
    call(35, 150, 0); // Let the child finish before waiting: exit must be retained.
    if (pid <= 0 || (i64)call(61, pid, (u64)&status) != pid || status != 7 ||
        (i64)call(61, pid, 0) != -1 || (i64)call(61, 1, 0) != -1 || (i64)call(61, 0, 0) != -1) {
        output("[wait-probe] FAILED\n"); call(60, 1, 0);
    }
    output("[wait-probe] passed fast exit, exit status, repeated and nonchild waits\n");
    call(60, 0, 0);
#endif
    for (;;) {}
}
