typedef unsigned long uint64_t;
typedef long          int64_t;

// Minimal syscall wrappers
static inline uint64_t sys_read(uint64_t fd, char* buf, uint64_t len) {
    uint64_t ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(0), "D"(fd), "S"(buf), "d"(len) : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t sys_write(uint64_t fd, const char* buf, uint64_t count) {
    uint64_t ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(1), "D"(fd), "S"(buf), "d"(count) : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t sys_open(const char* path, uint64_t flags) {
    uint64_t ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(2), "D"(path), "S"(flags) : "rcx", "r11", "memory");
    return ret;
}

static inline void sys_close(uint64_t fd) {
    __asm__ volatile ("syscall" : : "a"(3), "D"(fd) : "rcx", "r11", "memory");
}

static inline void sys_scroll(int64_t delta) {
    __asm__ volatile ("syscall" : : "a"(16), "D"(delta) : "rcx", "r11", "memory");
}

static inline void sys_ttyraw(uint64_t raw) {
    __asm__ volatile ("syscall" : : "a"(17), "D"(raw) : "rcx", "r11", "memory");
}

static inline void sys_reboot() {
    __asm__ volatile ("syscall" : : "a"(169) : "rcx", "r11", "memory");
    for(;;) {}
}

static inline void sys_shutdown() {
    __asm__ volatile ("syscall" : : "a"(88) : "rcx", "r11", "memory");
    for(;;) {}
}

int strlen(const char* s) {
    int len = 0; while(s[len]) len++; return len;
}

static int streq(const char* a, const char* b, int len) {
    for (int i = 0; i < len; i++) {
        if (a[i] == '\0' || b[i] == '\0') return (a[i] == b[i]);
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

void print(const char* s) { sys_write(1, s, strlen(s)); }

#define MAX_HISTORY 10
char history[MAX_HISTORY][128];
int history_count = 0;
int history_index = -1;

void add_history(const char* cmd) {
    if (strlen(cmd) == 0) return;
    for (int i = MAX_HISTORY - 1; i > 0; i--) {
        for (int j = 0; j < 128; j++) history[i][j] = history[i-1][j];
    }
    for (int j = 0; j < 127; j++) {
        history[0][j] = cmd[j];
        if (cmd[j] == '\0') break;
    }
    if (history_count < MAX_HISTORY) history_count++;
}

// Known commands for green highlighting
static const char* known_commands[] = {
    "clear", "shutdown", "restart", "open bootlog", 0
};

static int is_known_command(const char* buf, int len) {
    if (len == 0) return 0;
    
    // Check full string for multi-word commands
    for (int i = 0; known_commands[i]; i++) {
        int klen = strlen(known_commands[i]);
        if (len == klen && streq(buf, known_commands[i], klen)) return 1;
    }

    // Check first word for others
    int space_idx = -1;
    for (int i = 0; i < len; i++) { if (buf[i] == ' ') { space_idx = i; break; } }
    
    int cmd_len = (space_idx == -1) ? len : space_idx;
    for (int i = 0; known_commands[i]; i++) {
        int klen = strlen(known_commands[i]);
        // Don't match "open" against "open bootlog" here, it's handled above or by checking start
        if (cmd_len == klen && streq(buf, known_commands[i], klen)) return 1;
    }
    
    return 0;
}

// Redraw the current input line in-place with color feedback
static void redraw_line(const char* buf, int len) {
    // Move to start of line, reprint prompt + buffer
    // Use green if typing a valid command, default otherwise
    if (is_known_command(buf, len)) {
        print("\r\x1b[32mshell> ");   // green
    } else {
        print("\r\x1b[0mshell> ");    // default white
    }
    sys_write(1, buf, len);
    // Erase any leftover characters to the right (from a shorter previous entry)
    print("\x1b[K");
}


void _start() {
    print("\x1b[2J\x1b[H"); 
    print("\n----------------------------------------\n");
    print("Welcome to MyOS Interactive Terminal!\n");
    print("Shift + Up/Down: Hardware Scrollback\n");
    print("Up/Down: Command History\n");
    print("----------------------------------------\n");

    char buf[128];
    int pos = 0;

    while(1) {
        sys_ttyraw(1);          // Enter raw/interactive mode BEFORE the prompt
        print("\nshell> ");     // tty_raw_mode=1 → cursor appears here, nowhere during init
        pos = 0;
        history_index = -1;

        while (1) {
            char c;
            uint64_t n = sys_read(0, &c, 1);

            if (n == 0 || n == (uint64_t)-1) continue;

            if (c == '\n' || c == '\r') {
                sys_ttyraw(0);   // leave raw mode before command output
                buf[pos] = '\0';
                print("\x1b[0m\n");  // reset color, newline
                break;
            } else if (c == '\b' || c == 127) {
                if (pos > 0) {
                    pos--;
                    redraw_line(buf, pos);
                }
            } else if (c == '\x1b') {
                // Arrow key escape sequence: read next two bytes (still in raw mode)
                char seq[2];
                if (sys_read(0, &seq[0], 1) && sys_read(0, &seq[1], 1)) {
                    if (seq[0] == '[') {
                        if (seq[1] == 'A') { // Up arrow
                            if (history_count > 0 && history_index < history_count - 1) {
                                history_index++;
                                pos = 0;
                                while (history[history_index][pos]) {
                                    buf[pos] = history[history_index][pos]; pos++;
                                }
                                redraw_line(buf, pos);
                            }
                        } else if (seq[1] == 'B') { // Down arrow
                            if (history_index >= 0) {
                                history_index--;
                                pos = 0;
                                if (history_index >= 0) {
                                    while (history[history_index][pos]) {
                                        buf[pos] = history[history_index][pos]; pos++;
                                    }
                                }
                                redraw_line(buf, pos);
                            }
                        }
                    }
                }
            } else if (pos < 127) {
                buf[pos++] = c;
                redraw_line(buf, pos);
            }
        }

        if (pos == 0) continue;
        add_history(buf);

        if (streq(buf, "clear", 5)) {
            print("\x1b[2J\x1b[H");
        } else if (streq(buf, "shutdown", 8)) {
            sys_shutdown();
        } else if (streq(buf, "restart", 7)) {
            sys_reboot();
        } else if (streq(buf, "open bootlog", 12)) {
            uint64_t fd = sys_open("/bootlog.txt", 0);
            if (fd != (uint64_t)-1) {
                char logbuf[4096]; uint64_t b;
                while ((b = sys_read(fd, logbuf, 4096)) > 0) sys_write(1, logbuf, b);
                sys_close(fd);
            }
        } else {
            print("Unknown command: "); print(buf); print("\n");
        }
    }
}