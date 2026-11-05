typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef unsigned long  uint64_t;
typedef long           int64_t;

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
    print("[win_test] Surface synced to compositor. Starting animation loop...\n");
    
    // Smooth 30 FPS animated banner at the top of the window
    uint32_t frame = 0;
    while (1) {
        frame++;
        
        // Animated color wave bar in top 40 rows
        for (uint32_t y = 0; y < 32; y++) {
            for (uint32_t x = 0; x < 480; x++) {
                uint8_t wave = (uint8_t)((x + frame * 4) % 256);
                buf[y * 480 + x] = (0xFF << 24) | (wave << 16) | ((255 - wave) << 8) | 0xAA;
            }
        }
        
        sys_win_sync(win);
        sys_sleep(33); // ~30 FPS, sleep yields CPU so 0% idle load!
    }
}
