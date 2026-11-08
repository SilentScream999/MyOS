typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef int            int32_t;
typedef unsigned long  uint64_t;
typedef long           int64_t;

#define WIN_EVENT_NONE         0u
#define WIN_EVENT_MOUSE_DOWN   1u
#define WIN_EVENT_MOUSE_UP     2u
#define WIN_EVENT_MOUSE_MOVE   3u
#define WIN_EVENT_MOUSE_SCROLL 4u
#define WIN_EVENT_KEY_DOWN     5u
#define WIN_EVENT_KEY_UP       6u
#define WIN_EVENT_CLOSE        7u

typedef struct {
    uint32_t type;
    int32_t  x;
    int32_t  y;
    uint32_t key;
    uint32_t mods;
    int32_t  delta;
} WinEvent;

// Minimal syscall wrappers for window management
static inline uint64_t sys_win_create(const char* title, uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint64_t flags) {
    uint64_t ret;
    register uint64_t r10_val __asm__("r10") = w;
    register uint64_t r8_val  __asm__("r8")  = h;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"((uint64_t)200), "D"(title), "S"(x), "d"(y), "r"(r10_val), "r"(r8_val)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline void sys_win_sync(uint64_t id) {
    __asm__ volatile ("syscall" : : "a"((uint64_t)201), "D"(id) : "rcx", "r11", "memory");
}

static inline uint64_t sys_win_get_buffer(uint64_t id) {
    uint64_t ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"((uint64_t)203), "D"(id) : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t sys_win_poll_event(uint64_t id, WinEvent* ev) {
    uint64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"((uint64_t)205), "D"(id), "S"(ev)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline void sys_sleep(uint64_t ms) {
    __asm__ volatile ("syscall" : : "a"((uint64_t)35), "D"(ms) : "rcx", "r11", "memory");
}

static inline void __attribute__((noreturn)) sys_exit(uint64_t code) {
    __asm__ volatile ("syscall" : : "a"((uint64_t)60), "D"(code) : "rcx", "r11", "memory");
    for (;;) {}
}

static inline void sys_write(uint64_t fd, const char* s, uint64_t len) {
    __asm__ volatile ("syscall" : : "a"(1), "D"(fd), "S"(s), "d"(len) : "rcx", "r11", "memory");
}

static int strlen(const char* s) {
    int len = 0; while (s[len]) len++; return len;
}

void print(const char* s) {
    sys_write(1, s, strlen(s));
}

void _start() {
    print("[win_test] Starting Ring 3 Window Application...\n");
    
    uint64_t win = sys_win_create("Wyzar Desktop Demo", 220, 160, 480, 320, 1);
    if (!win) {
        print("[win_test] Error: Failed to create window!\n");
        sys_exit(1);
    }
    
    print("[win_test] Window created! Acquiring user-mapped surface...\n");
    
    uint32_t* buf = (uint32_t*)sys_win_get_buffer(win);
    if (!buf) {
        print("[win_test] Error: Failed to map window surface buffer!\n");
        sys_exit(1);
    }
    
    print("[win_test] Surface mapped successfully! Rendering pattern...\n");
    
    // Draw initial smooth color gradient across the surface
    for (uint32_t y = 0; y < 320; y++) {
        for (uint32_t x = 0; x < 480; x++) {
            uint8_t r = (uint8_t)(x * 255 / 480);
            uint8_t g = (uint8_t)(y * 255 / 320);
            uint8_t b = 180;
            buf[y * 480 + x] = (0xFF << 24) | (r << 16) | (g << 8) | b;
        }
    }
    
    sys_win_sync(win);
    print("[win_test] Interactive event loop started. (Click to paint, keys R/G/B/Y/C to change color/clear)\n");
    
    uint32_t frame = 0;
    uint32_t draw_color = 0xFFFFFFFF; // White brush by default
    int32_t color_shift = 0;

    while (1) {
        frame++;
        
        // Poll and process pending events
        WinEvent ev;
        while (sys_win_poll_event(win, &ev)) {
            if (ev.type == WIN_EVENT_CLOSE) {
                // Normal process exit destroys owned windows and wakes the shell.
                sys_exit(0);
            } else if (ev.type == WIN_EVENT_MOUSE_DOWN) {
                // Click paints a stamp at cursor
                for (int dy = -8; dy <= 8; dy++) {
                    int py = ev.y + dy;
                    if (py < 32 || py >= 320) continue;
                    for (int dx = -8; dx <= 8; dx++) {
                        int px = ev.x + dx;
                        if (px < 0 || px >= 480) continue;
                        buf[py * 480 + px] = draw_color;
                    }
                }
            } else if (ev.type == WIN_EVENT_MOUSE_SCROLL) {
                color_shift += ev.delta * 16;
            } else if (ev.type == WIN_EVENT_KEY_DOWN) {
                if (ev.key == 'c' || ev.key == 'C') {
                    for (uint32_t y = 32; y < 320; y++) {
                        for (uint32_t x = 0; x < 480; x++) {
                            uint8_t r = (uint8_t)(x * 255 / 480);
                            uint8_t g = (uint8_t)(y * 255 / 320);
                            uint8_t b = (uint8_t)((180 + color_shift) % 256);
                            buf[y * 480 + x] = (0xFF << 24) | (r << 16) | (g << 8) | b;
                        }
                    }
                } else if (ev.key == 'r' || ev.key == 'R') {
                    draw_color = 0xFFFF3333;
                } else if (ev.key == 'g' || ev.key == 'G') {
                    draw_color = 0xFF33FF33;
                } else if (ev.key == 'b' || ev.key == 'B') {
                    draw_color = 0xFF3388FF;
                } else if (ev.key == 'y' || ev.key == 'Y') {
                    draw_color = 0xFFFFFF33;
                } else {
                    draw_color = 0xFFFFFFFF;
                }
            }
        }
        
        // Animated color wave bar in top 32 rows
        for (uint32_t y = 0; y < 32; y++) {
            for (uint32_t x = 0; x < 480; x++) {
                uint8_t wave = (uint8_t)((x + frame * 4) % 256);
                buf[y * 480 + x] = (0xFF << 24) | (wave << 16) | ((255 - wave) << 8) | 0xAA;
            }
        }
        
        sys_win_sync(win);
        sys_sleep(33); // ~30 FPS, yields CPU
    }
}
