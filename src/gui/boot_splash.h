#ifndef BOOT_SPLASH_H
#define BOOT_SPLASH_H

// Early boot has neither a scheduler nor a working IRQ timer yet. Pump this
// small animation from boot's wait loops using the TSC, never an extra delay.
extern volatile bool g_desktop_ready;
static volatile bool g_boot_splash_active = false;
static uint64_t g_boot_splash_last = 0;
static uint64_t g_boot_splash_frame_ticks = 1;
static uint32_t g_boot_splash_phase = 0;
static constexpr uint32_t BOOT_SPLASH_BG = 0xFF0A0F1E;

static uint32_t boot_splash_blend(uint32_t from, uint32_t to, uint32_t alpha) {
    uint32_t color = 0xFF000000;
    for (uint32_t shift = 0; shift < 24; shift += 8) {
        uint32_t a = (from >> shift) & 255, b = (to >> shift) & 255;
        color |= ((a * (256 - alpha) + b * alpha) >> 8) << shift;
    }
    return color;
}

// Integer octant approximation; avoids floating point before FPU task state
// exists. Angles increase clockwise in screen coordinates, in 1/256 turns.
static uint32_t boot_splash_angle(int32_t x, int32_t y) {
    uint32_t ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    uint32_t angle = ax >= ay ? (ax ? ay * 32 / ax : 0) : 64 - ax * 32 / ay;
    if (x < 0) angle = 128 - angle;
    if (y < 0) angle = 256 - angle;
    return angle & 255;
}

static void boot_splash_render(uint32_t phase) {
    int32_t radius = (int32_t)(fb->height / 54);
    if (radius < 18) radius = 18;
    if (radius > 28) radius = 28;
    int32_t cx = fb->width / 2, cy = fb->height / 2;
    int32_t extent = radius + 3;
    // A two-pixel ring, with a one-pixel antialiased edge. Only this small
    // square is touched each frame; no full-screen clear or heap allocation.
    for (int32_t y = -extent; y <= extent; y++) {
        for (int32_t x = -extent; x <= extent; x++) {
            int32_t radial = ((x*x + y*y - radius*radius) * 128) / radius;
            if (radial < 0) radial = -radial;
            int32_t coverage = 384 - radial;
            uint32_t color = BOOT_SPLASH_BG;
            if (coverage > 0) {
                if (coverage > 256) coverage = 256;
                uint32_t trail = (phase - boot_splash_angle(x, y)) & 255;
                uint32_t arc = trail < 80 ? (80 - trail) * 256 / 80 : 0;
                uint32_t ring = boot_splash_blend(0xFF283248, 0xFFC6D6EE, arc);
                color = boot_splash_blend(BOOT_SPLASH_BG, ring, coverage);
            }
            if (cx + x >= 0 && cy + y >= 0)
                vram_putpx(cx + x, cy + y, color);
        }
    }
    vram_fence();
}

void boot_splash_tick() {
    if (!g_boot_splash_active || g_fatal_screen_visible) return;
    uint64_t now = rdtsc();
    if (now - g_boot_splash_last < g_boot_splash_frame_ticks) return;
    // Lock the handoff against preemption. Once the compositor starts, no old
    // splash frame may paint over its desktop or over a fatal error screen.
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    if (g_boot_splash_active && !g_fatal_screen_visible) {
        uint64_t frames = (now - g_boot_splash_last) / g_boot_splash_frame_ticks;
        g_boot_splash_last = now;
        g_boot_splash_phase = (g_boot_splash_phase + frames * 6) & 255;
        boot_splash_render(g_boot_splash_phase);
    }
    if (flags & (1u << 9)) __asm__ volatile("sti" ::: "memory");
}

static void boot_splash_begin() {
    if (!fb || !fb->address || fb->bpp != 32) return;
    g_boot_splash_frame_ticks = _usb_tsc_hz() / 30;
    if (!g_boot_splash_frame_ticks) g_boot_splash_frame_ticks = 1;
    g_boot_splash_last = rdtsc();
    g_boot_splash_phase = 192;
    g_boot_splash_active = true;
    vram_fill(BOOT_SPLASH_BG);
    boot_splash_render(g_boot_splash_phase);
}

// The compositor has already prepared the entire desktop offscreen. Retire
// the splash and present that complete image in one contiguous streaming copy,
// without scheduler slices exposing a partly replaced splash for many frames.
// GOP provides a visible framebuffer, not an atomic hardware page flip.
static bool boot_splash_present_desktop(const uint8_t* desktop) {
    if (!desktop || !fb || !fb->address || g_fatal_screen_visible) return false;
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    if (g_fatal_screen_visible) {
        if (flags & (1u << 9)) __asm__ volatile("sti" ::: "memory");
        return false;
    }
    g_boot_splash_active = false;
    memcpy_vram_sse_headless(fb->address, desktop, fb->pitch * fb->height);
    vram_fence();
    if (flags & (1u << 9)) __asm__ volatile("sti" ::: "memory");
    return true;
}

static void boot_loading_screen_until_ready() {
    while (!g_desktop_ready) {
        boot_splash_tick();
        __asm__ volatile("pause");
    }
}

#endif
