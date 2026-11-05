typedef unsigned long uint64_t;

static inline uint64_t sys_write(uint64_t fd, const char* buf, uint64_t count) {
    uint64_t ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(1), "D"(fd), "S"(buf), "d"(count) : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t sys_getpid() {
    uint64_t ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(39) : "rcx", "r11", "memory");
    return ret;
}

// SYS_SLEEP (35): block for `ms` milliseconds, yielding CPU entirely.
static inline void sys_sleep(uint64_t ms) {
    __asm__ volatile ("syscall" : : "a"((uint64_t)35), "D"(ms) : "rcx", "r11", "memory");
}

static inline void __attribute__((noreturn)) sys_exit(uint64_t code) {
    __asm__ volatile ("syscall" : : "a"(60), "D"(code) : "rcx", "r11", "memory");
    for (;;) {}
}

static int strlen(const char* s) {
    int len = 0; while (s[len]) len++; return len;
}

static void print(const char* s) {
    sys_write(1, s, strlen(s));
}

static void print_num(uint64_t n) {
    char buf[24];
    int idx = 0;
    if (n == 0) { buf[idx++] = '0'; }
    else {
        char tmp[24]; int t = 0;
        while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
        while (t > 0) buf[idx++] = tmp[--t];
    }
    buf[idx] = '\0';
    print(buf);
}

void _start() {
    uint64_t pid = sys_getpid();
    print("\n[counter.elf] Started! Running in Ring 3 as PID ");
    print_num(pid);
    print("\n");

    for (uint64_t i = 1; i <= 5; i++) {
        print("[counter.elf PID ");
        print_num(pid);
        print("] Tick ");
        print_num(i);
        print(" of 5\n");

        // Block the task for 500 ms — the scheduler gives the CPU to other
        // tasks while we sleep, so CPU usage drops to near zero.
        sys_sleep(500);
    }

    print("[counter.elf] Done! Exiting cleanly via sys_exit.\n");
    sys_exit(0);
}
