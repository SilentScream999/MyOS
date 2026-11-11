typedef unsigned long uint64_t;
typedef long          int64_t;
#include "src/fs/fileinfo.h"
#include "src/core/app.h"

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
static inline int64_t sys_lseek(uint64_t fd, int64_t offset, uint64_t whence) {
    int64_t result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(8UL), "D"(fd), "S"(offset), "d"(whence) : "rcx", "r11", "memory");
    return result;
}
static inline int64_t sys_unlink(const char* path) {
    int64_t result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(87UL), "D"(path) : "rcx", "r11", "memory");
    return result;
}
static inline int64_t sys_stat(const char* path, file_info* info) {
    int64_t result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(4UL), "D"(path), "S"(info) : "rcx", "r11", "memory");
    return result;
}
static inline int64_t sys_getcwd(char* buf, uint64_t size) {
    int64_t result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(79UL), "D"(buf), "S"(size) : "rcx", "r11", "memory");
    return result;
}
static inline int64_t sys_chdir(const char* path) {
    int64_t result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(80UL), "D"(path) : "rcx", "r11", "memory");
    return result;
}
static inline int64_t sys_mkdir(const char* path, uint64_t mode) {
    int64_t result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(83UL), "D"(path), "S"(mode) : "rcx", "r11", "memory");
    return result;
}
static inline int64_t sys_rmdir(const char* path) {
    int64_t result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(84UL), "D"(path) : "rcx", "r11", "memory");
    return result;
}
static inline int64_t sys_statfs(const char* path, fs_info* info) {
    int64_t result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(137UL), "D"(path), "S"(info) : "rcx", "r11", "memory");
    return result;
}

static inline void sys_scroll(int64_t delta) {
    __asm__ volatile ("syscall" : : "a"(16), "D"(delta) : "rcx", "r11", "memory");
}

static inline void sys_ttyraw(uint64_t raw) {
    __asm__ volatile ("syscall" : : "a"(17), "D"(raw) : "rcx", "r11", "memory");
}
static inline void sys_ttyecho(uint64_t enabled) {
    uint64_t result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(18UL), "D"(enabled) : "rcx", "r11", "memory");
}
static inline void sys_ttyflush_input(void) {
    uint64_t result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(19UL) : "rcx", "r11", "memory");
}
static inline int64_t sys_take_launch(char* path, uint64_t capacity) {
    int64_t result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(20UL), "D"(path), "S"(capacity) : "rcx", "r11", "memory");
    return result;
}

static inline int64_t sys_readdir(const char* path, uint64_t index, char* name_out, uint64_t* size_out, uint64_t* type_out) {
    int64_t ret;
    // SysV AMD64: a4→r10, a5→r8 (rcx/r11 clobbered by syscall instruction).
    // We use named register variables to unambiguously bind size_out→r10,
    // type_out→r8 so the compiler cannot mis-assign them.
    register uint64_t r10_val __asm__("r10") = (uint64_t)size_out;
    register uint64_t r8_val  __asm__("r8")  = (uint64_t)type_out;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"((uint64_t)40), "D"(path), "S"(index), "d"(name_out),
          "r"(r10_val), "r"(r8_val)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline int64_t sys_spawn(const char* path) {
    int64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"((uint64_t)59), "D"(path)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline void sys_sleep(uint64_t ms) {
    __asm__ volatile (
        "syscall"
        :
        : "a"((uint64_t)35), "D"(ms)
        : "rcx", "r11", "memory"
    );
}
static inline int64_t sys_waitpid(uint64_t pid, uint64_t* code) {
    int64_t result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(61UL), "D"(pid), "S"(code) : "rcx", "r11", "memory");
    return result;
}

static inline void sys_reboot() {
    uint64_t result;
    __asm__ volatile ("syscall" : "=a"(result) : "a"(169) : "rcx", "r11", "memory");
}

static inline void sys_shutdown() {
    uint64_t result;
    __asm__ volatile ("syscall" : "=a"(result) : "a"(88) : "rcx", "r11", "memory");
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

void print_dec(uint64_t val) {
    if (val == 0) {
        print("0");
        return;
    }
    char buf[24];
    int i = 0;
    while (val > 0) {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    }
    for (int j = 0; j < i / 2; j++) {
        char tmp = buf[j];
        buf[j] = buf[i - 1 - j];
        buf[i - 1 - j] = tmp;
    }
    buf[i] = '\0';
    print(buf);
}

static void print_two(uint64_t value) {
    if (value < 10) print("0");
    print_dec(value);
}
static void print_datetime(const file_datetime* t) {
    if (!t->year) { print("unknown"); return; }
    print_dec(t->year); print("-"); print_two(t->month); print("-"); print_two(t->day);
    print(" "); print_two(t->hour); print(":"); print_two(t->minute); print(":"); print_two(t->second & ~1u);
}

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
    "clear", "shutdown", "restart", "bsod", "open bootlog", "ls", "run", "help",
    "write", "cat", "rm", "stat", "cd", "pwd", "mkdir", "rmdir", "df", 0
};

static char current_dir[256] = "/";

static void update_cwd() {
    if (!sys_getcwd(current_dir, sizeof(current_dir))) {
        current_dir[0] = '/';
        current_dir[1] = '\0';
    }
}

static void print_size_mb(uint64_t bytes) {
    if (bytes >= 1024 * 1024 * 1024) {
        uint64_t gb = bytes / (1024 * 1024 * 1024);
        print_dec(gb);
        print("G");
    } else if (bytes >= 1024 * 1024) {
        uint64_t mb = bytes / (1024 * 1024);
        print_dec(mb);
        print("M");
    } else if (bytes >= 1024) {
        uint64_t kb = bytes / 1024;
        print_dec(kb);
        print("K");
    } else {
        print_dec(bytes);
        print("B");
    }
}

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
        if (cmd_len == klen && streq(buf, known_commands[i], klen)) return 1;
    }
    return 0;
}

// Redraw the current input line in-place with color feedback
static void redraw_line(const char* buf, int len) {
    if (is_known_command(buf, len)) {
        print("\r\x1b[32mwyzar:");   // green
    } else {
        print("\r\x1b[0mwyzar:");    // default white
    }
    print(current_dir);
    print("> \x1b[0m");
    sys_write(1, buf, len);
    // Erase any leftover characters to the right
    print("\x1b[K");
}

void _start() {
    print("\x1b[2J\x1b[H"); 
    print("\n----------------------------------------\n");
    print("Welcome to WyzarOS Interactive Terminal!\n");
    print("Shift + Up/Down: Hardware Scrollback\n");
    print("Up/Down: Command History\n");
    print("Type 'help' for available commands\n");
    print("----------------------------------------\n");

    char buf[128];
    int pos = 0;

    while(1) {
        update_cwd();
        sys_ttyecho(1);
        sys_ttyraw(1);          // Enter raw/interactive mode BEFORE the prompt
        print("\nwyzar:");
        print(current_dir);
        print("> ");     // tty_raw_mode=1 -> cursor appears here
        pos = 0;
        history_index = -1;

        while (1) {
            char c;
            uint64_t n = sys_read(0, &c, 1);

            if (n == (uint64_t)-2) {
                buf[0] = 'r'; buf[1] = 'u'; buf[2] = 'n'; buf[3] = ' ';
                if (sys_take_launch(buf + 4, sizeof(buf) - 4) <= 0) continue;
                pos = strlen(buf);
                sys_ttyraw(0);
                print("\r\x1b[2Kwyzar:"); print(current_dir); print("> "); print(buf); print("\n");
                break;
            }

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

        if (streq(buf, "help", 4) && (buf[4] == ' ' || buf[4] == '\0')) {
            print("Available commands:\n");
            print("  cd [path]       - Change directory (default '/')\n");
            print("  pwd             - Print current working directory\n");
            print("  ls [path]       - List directory contents (default '.')\n");
            print("  mkdir <path>    - Create a new directory\n");
            print("  rmdir <path>    - Remove an empty directory\n");
            print("  df              - Show filesystem disk space usage\n");
            print("  write <path> <text> - Create/write a file; disk root is persistent\n");
            print("  cat <path>      - Read a file\n");
            print("  rm <path>       - Delete a closed file\n");
            print("  stat <path>     - Show size, creation and modification times\n");
            print("  run <path>      - Run a .wiz or .elf app; wait until it exits\n");
            print("  open bootlog    - Display kernel boot log\n");
            print("  clear           - Clear screen\n");
            print("  bsod            - Trigger crash screen\n");
            print("  restart         - Reboot system\n");
            print("  shutdown        - Power off system\n");
        } else if (streq(buf, "cd", 2) && (buf[2] == ' ' || buf[2] == '\0')) {
            const char* target = "/";
            if (buf[2] == ' ') {
                int idx = 2;
                while (buf[idx] == ' ') idx++;
                if (buf[idx] != '\0' && !streq(&buf[idx], "~", 1)) target = &buf[idx];
            }
            if (sys_chdir(target) == 0) {
                update_cwd();
            } else {
                print("cd: no such directory: "); print(target); print("\n");
            }
        } else if (streq(buf, "pwd", 3) && (buf[3] == ' ' || buf[3] == '\0')) {
            update_cwd();
            print(current_dir); print("\n");
        } else if (streq(buf, "mkdir", 5) && buf[5] == ' ') {
            int idx = 5;
            while (buf[idx] == ' ') idx++;
            if (buf[idx] == '\0') {
                print("Usage: mkdir <path>\n");
            } else if (sys_mkdir(&buf[idx], 0755) == 0) {
                print("Directory created.\n");
            } else {
                print("Cannot create directory. Check path, storage, or duplicate name.\n");
            }
        } else if (streq(buf, "rmdir", 5) && buf[5] == ' ') {
            int idx = 5;
            while (buf[idx] == ' ') idx++;
            if (buf[idx] == '\0') {
                print("Usage: rmdir <path>\n");
            } else if (sys_rmdir(&buf[idx]) == 0) {
                print("Directory removed.\n");
            } else {
                print("Cannot remove directory. Ensure directory exists and is empty.\n");
            }
        } else if (streq(buf, "df", 2) && (buf[2] == ' ' || buf[2] == '\0')) {
            fs_info st;
            if (sys_statfs("/", &st) == 0) {
                print("Filesystem     Type    Size    Used    Free   Use% Mounted on\n");
                print("/dev/root      ");
                print(st.fs_type);
                int pad = 8 - strlen(st.fs_type);
                while (pad-- > 0) print(" ");
                print_size_mb(st.total_bytes);
                print("    ");
                uint64_t used = st.total_bytes >= st.free_bytes ? st.total_bytes - st.free_bytes : 0;
                print_size_mb(used);
                print("    ");
                print_size_mb(st.free_bytes);
                print("   ");
                uint64_t pct = st.total_bytes ? (used * 100) / st.total_bytes : 0;
                print_dec(pct);
                print("%   /\n");
            } else {
                print("df: failed to get filesystem statistics\n");
            }
        } else if (streq(buf, "rm", 2) && buf[2] == ' ') {
            int idx = 3; while (buf[idx] == ' ') idx++;
            if (sys_unlink(&buf[idx]) == 0) print("File deleted.\n");
            else print("Cannot delete: check path, open handles, read-only attribute and storage. Only short-name files are supported.\n");
        } else if (streq(buf, "stat", 4) && buf[4] == ' ') {
            int idx = 5; while (buf[idx] == ' ') idx++;
            file_info info;
            if (sys_stat(&buf[idx], &info) != 0) { print("Cannot stat file.\n"); continue; }
            print("Size: "); print_dec(info.size); print(" bytes\n");
            print("Created:  "); print_datetime(&info.created); print("\n");
            print("Modified: "); print_datetime(&info.modified); print("\n");
            print("Times shown to 2-second precision; quick writes may have equal times.\n");
        } else if (streq(buf, "write", 5) && buf[5] == ' ') {
            int idx = 6;
            while (buf[idx] == ' ') idx++;
            char* path = &buf[idx];
            while (buf[idx] && buf[idx] != ' ') idx++;
            if (!buf[idx]) { print("Usage: write <path> <text>\n"); continue; }
            buf[idx++] = 0;
            while (buf[idx] == ' ') idx++;
            uint64_t fd = sys_open(path, 0x41); // O_WRONLY | O_CREAT
            if (fd == (uint64_t)-1) {
                print("Cannot open file. Check that the parent directory exists and storage is online.\n");
                print("Check the boot log for disk root or RAM recovery mode. Disk names must fit 8.3.\n");
                continue;
            }
            uint64_t len = strlen(&buf[idx]);
            uint64_t written = sys_write(fd, &buf[idx], len);
            sys_close(fd);
            if (written == len) print("Write complete. Existing files keep any trailing bytes.\n");
            else print("Write failed or incomplete.\n");
        } else if (streq(buf, "cat", 3) && buf[3] == ' ') {
            int idx = 4;
            while (buf[idx] == ' ') idx++;
            uint64_t fd = sys_open(&buf[idx], 0);
            if (fd == (uint64_t)-1) { print("Cannot open file.\n"); continue; }
            char data[512]; uint64_t bytes;
            while ((bytes = sys_read(fd, data, sizeof(data))) > 0 && bytes <= sizeof(data)) sys_write(1, data, bytes);
            sys_close(fd); print("\n");
        } else if (streq(buf, "ls", 2) && (buf[2] == ' ' || buf[2] == '\0')) {
            const char* path = ".";
            if (buf[2] == ' ') {
                int idx = 2;
                while (buf[idx] == ' ') idx++;
                if (buf[idx] != '\0') path = &buf[idx];
            }
            char name[256];
            uint64_t sz = 0;
            uint64_t type = 0;
            uint64_t idx = 0;
            int count = 0;
            while (sys_readdir(path, idx, name, &sz, &type) > 0) {
                if (type == 2) {
                    print("\x1b[34m[DIR]  \x1b[0m");
                    print(name);
                    print("/\n");
                } else {
                    print("\x1b[32m[FILE] \x1b[0m");
                    print(name);
                    print(" (");
                    print_dec(sz);
                    print(" bytes)\n");
                }
                idx++;
                count++;
            }
            if (count == 0) {
                print("Directory is empty or does not exist: ");
                print(path);
                print("\n");
            }
        } else if (streq(buf, "run", 3) && buf[3] == ' ') {
            int idx = 3;
            while (buf[idx] == ' ') idx++;
            if (buf[idx] != '\0') {
                const char* prog = &buf[idx];
                if (app_path_is_shell(prog)) {
                    print("Refused: Cannot spawn a second interactive init shell on the same TTY.\n");
                    continue;
                }
                print("Spawning: "); print(prog); print("...\n");
                sys_ttyecho(0); // child may run as soon as spawn returns
                int64_t pid = sys_spawn(prog);
                if (pid > 0) {
                    print("Successfully spawned process with PID ");
                    print_dec(pid);
                    print("\n");
                    uint64_t exit_code = 0;
                    if (sys_waitpid(pid, &exit_code) < 0) {
                        print("Failed to wait for program.\n");
                    } else if (exit_code == 130) {
                        print("^C\n");
                    } else if (exit_code != 0) {
                        print("Program exited with code "); print_dec(exit_code); print("\n");
                    }
                } else {
                    print("Failed to spawn process (error ");
                    print_dec(-pid);
                    print(")\n");
                }
                sys_ttyraw(0);
                sys_ttyflush_input(); // discard keys the finished program never consumed
                sys_ttyecho(1);
            } else {
                print("Usage: run <path>\n");
            }
        } else if (streq(buf, "clear", 5)) {
            print("\x1b[2J\x1b[H");
        } else if (streq(buf, "bsod", 4)) {
            print("Triggering BSOD...\n");
            __asm__ volatile ("ud2");
        } else if (streq(buf, "shutdown", 8)) {
            sys_shutdown();
            print("Shutdown request returned without powering off.\n");
        } else if (streq(buf, "restart", 7)) {
            sys_reboot();
            print("Restart request returned without restarting.\n");
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
