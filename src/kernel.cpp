#include <stddef.h>
#include <stdbool.h>

#include "helpers.h"
#include "pagingstuff.h"
#include "usbhelpers.h"

#include "gdt.h"
#include "idt.h"

#include "irq.h"    // PIC remapping + irq_register
#include "timer.h"  // PIT driver + g_tick_count
#include "heap.h"
#include "tty.h"
#include "acpi.h"
#include "syscall.h"
#include "ramfs.h"
#include "exec.h"
#include "graphics.h"
#include "wm.h"

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(4);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request fb_req = {
	.id = LIMINE_FRAMEBUFFER_REQUEST,
	.revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_req = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_efi_system_table_request efi_st_req = {
    .id = LIMINE_EFI_SYSTEM_TABLE_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_stack_size_request stack_size_req = {
	.id        = LIMINE_STACK_SIZE_REQUEST,
	.revision  = 0,
	.stack_size = 4 * 1024 * 1024
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_smp_request smp_req = {
    .id = LIMINE_SMP_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;
__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

struct limine_framebuffer *fb = nullptr;

// ── TSC helpers ───────────────────────────────────────────────────────────────
//
// We use the invariant TSC as a wall-clock substitute before any timer driver
// is available — the same technique Linux uses in early boot
// (arch/x86/kernel/tsc.c, tsc_early_delay_calibrate).
//
// rdtsc() returns raw TSC ticks.  get_tsc_freq_hz() figures out how many ticks
// equal one second so we can express repeat thresholds in real milliseconds
// rather than uncalibrated loop iterations.

static inline uint64_t rdtsc() {
	uint32_t lo, hi;
	__asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
}

// ── Boot splash (loading screen) ─────────────────────────────────────────────
// For testing, this intentionally loops forever so you can observe animation.
static inline void vram_putpx(uint32_t x, uint32_t y, uint32_t argb) {
	if (!fb || !fb->address) return;
	if (x >= fb->width || y >= fb->height) return;
	uint32_t* row = (uint32_t*)((uint8_t*)fb->address + (uint64_t)y * fb->pitch);
	row[x] = argb;
}

static inline void vram_fill(uint32_t argb) {
	if (!fb || !fb->address) return;
	for (uint32_t y = 0; y < fb->height; y++) {
		uint32_t* row = (uint32_t*)((uint8_t*)fb->address + (uint64_t)y * fb->pitch);
		memset_32(row, argb, fb->width);
	}
	vram_fence();
}

static inline void vram_fill_circle(int32_t cx, int32_t cy, int32_t r, uint32_t argb) {
	if (!fb || !fb->address) return;
	int32_t r2 = r * r;
	for (int32_t dy = -r; dy <= r; dy++) {
		int32_t y = cy + dy;
		if (y < 0 || (uint32_t)y >= fb->height) continue;
		uint32_t* row = (uint32_t*)((uint8_t*)fb->address + (uint64_t)y * fb->pitch);
		for (int32_t dx = -r; dx <= r; dx++) {
			if (dx*dx + dy*dy > r2) continue;
			int32_t x = cx + dx;
			if (x < 0 || (uint32_t)x >= fb->width) continue;
			row[x] = argb;
		}
	}
}

static inline void vram_draw_char_8x8(int32_t x, int32_t y, char c, uint32_t argb) {
	if (!fb || !fb->address) return;
	if (c < 0 || c > 127) return;
	const uint8_t* glyph = font8x8_basic[(uint8_t)c];
	for (int32_t r = 0; r < 8; r++) {
		int32_t yy = y + r;
		if (yy < 0 || (uint32_t)yy >= fb->height) continue;
		uint32_t* row = (uint32_t*)((uint8_t*)fb->address + (uint64_t)yy * fb->pitch);
		uint8_t bits = glyph[r];
		for (int32_t b = 0; b < 8; b++) {
			if (!(bits & (1u << b))) continue;
			int32_t xx = x + b;
			if (xx < 0 || (uint32_t)xx >= fb->width) continue;
			row[xx] = argb;
		}
	}
}

static inline void vram_draw_string_8x8(int32_t x, int32_t y, const char* s, uint32_t argb) {
	int32_t cx = x;
	for (int i = 0; s && s[i]; i++) {
		vram_draw_char_8x8(cx, y, s[i], argb);
		cx += 8;
	}
}

// 0..64 quarter-wave sine lookup (sin(0..pi/2)), scaled to Q15 (32767 = 1.0).
// Generated once and hardcoded to avoid floating point / libm.
static const int16_t k_sin_q15_qtr[65] = {
	0, 804, 1608, 2410, 3212, 4011, 4808, 5602,
	6393, 7180, 7962, 8740, 9512, 10279, 11039, 11793,
	12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
	18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
	23170, 23732, 24279, 24812, 25330, 25833, 26319, 26790,
	27245, 27683, 28105, 28510, 28898, 29269, 29622, 29957,
	30275, 30574, 30855, 31118, 31362, 31588, 31794, 31982,
	32151, 32301, 32431, 32543, 32635, 32708, 32761, 32767,
	32767
};

static inline int16_t sin_q15_u8(uint8_t a) {
	// Map a in [0,255] to sin(2pi * a/256) in Q15.
	uint8_t q = (a >> 6) & 3;     // quadrant 0..3
	uint8_t o = a & 63;           // offset within quadrant (0..63)
	uint8_t idx = (q & 1) ? (uint8_t)(64 - o) : o;
	int16_t v = k_sin_q15_qtr[idx];
	return (q >= 2) ? (int16_t)-v : v;
}

static inline int16_t cos_q15_u8(uint8_t a) {
	return sin_q15_u8((uint8_t)(a + 64)); // +pi/2
}

extern volatile bool g_desktop_ready;  // defined later in this TU

static void boot_loading_screen_until_ready() {
	if (!fb || !fb->address) return;

	// Static background (Windows-like dark blue/black)
	const uint32_t bg = 0xFF0A0F1E;

	const int32_t cx = (int32_t)fb->width / 2;
	const int32_t cy = (int32_t)fb->height / 2;
	const int32_t radius = 24;
	const int32_t dot_r = 3;

	// Angle in "turn/256" units, but with fractional precision (Q16.16).
	uint32_t ang_fp = 0;
	uint8_t speed_phase = 0;

	uint64_t target = rdtsc();
	uint64_t step = (tsc_hz ? (tsc_hz / 60) : 50000000ULL);

	while (!g_desktop_ready) {
		// Clear frame
		vram_fill(bg);

		// Windows 11-ish spinner:
		// - A short arc ("half circle") of dots
		// - Smooth rotation
		// - Speed varies: accelerates then decelerates (sinusoidal speed curve)
		//
		// Speed curve: omega = base + amp * (0.5 + 0.5*sin())
		// (implemented in integer math, Q16.16 on angle)
		{
			int16_t s = sin_q15_u8(speed_phase);        // [-32767, 32767]
			uint32_t ease = (uint32_t)(s + 32767);      // [0, 65534]
			// base/amp in (turn/256) per frame, expressed in Q16.16
			uint32_t omega_base = (2u << 16);           // ~2/256 turns per frame
			uint32_t omega_amp  = (5u << 16);           // add up to ~5/256 turns per frame
			uint32_t omega = omega_base + (uint32_t)((omega_amp * ease) / 65534u);
			ang_fp += omega;
			speed_phase = (uint8_t)(speed_phase + 3);   // controls accel/decel cadence
		}

		uint8_t ang_u8 = (uint8_t)(ang_fp >> 16);

		// Dot arc: 8 dots spanning ~180 degrees (pi = 128 in u8-turns)
		const uint32_t dot_count = 8;
		const uint32_t arc_span = 128; // half circle

		for (uint32_t i = 0; i < dot_count; i++) {
			// Position along arc: head at i=0, tail at i=dot_count-1
			uint32_t off = (dot_count <= 1) ? 0 : (i * arc_span) / (dot_count - 1);

			// Per-dot "ease": give each dot a slightly different phase so it
			// accelerates/decelerates a bit independently (Windows 11-ish feel).
			//
			// Keep wobble subtle and monotonic so dots never cross.
			// wobble is scaled down near the head/tail to preserve the arc endpoints.
			uint8_t dot_phase = (uint8_t)(speed_phase + (uint8_t)(i * 19u));
			int16_t wob = sin_q15_u8(dot_phase); // [-32767, 32767]
			uint32_t edge = (i <= (dot_count - 1u) / 2u) ? i : (dot_count - 1u - i);
			// edge in [0..3] for dot_count=8 → scale factor in [0..255]
			uint32_t edge_scale = (dot_count <= 1) ? 0u : (edge * 255u) / ((dot_count - 1u) / 2u);
			// amplitude in angle-units (0..255 is full circle); keep small (~6 units).
			int32_t wob_amp = (int32_t)(6 * (int32_t)edge_scale) / 255;
			int32_t wob_u8 = (int32_t)((int32_t)wob * wob_amp / 32767);

			uint8_t a = (uint8_t)(ang_u8 + (uint8_t)off + (uint8_t)wob_u8);

			int16_t cs = cos_q15_u8(a);
			int16_t sn = sin_q15_u8(a);
			int32_t px = cx + (int32_t)((int64_t)cs * radius / 32767);
			int32_t py = cy + (int32_t)((int64_t)sn * radius / 32767);

			// Brightness falloff along the arc (visual head brightest).
			// If the arc direction appears reversed, invert the gradient.
			uint32_t j = (dot_count - 1u) - i;
			uint32_t v = (j == 0) ? 255 :
			             (j == 1) ? 210 :
			             (j == 2) ? 170 :
			             (j == 3) ? 135 :
			             (j == 4) ? 110 :
			             (j == 5) ? 95  :
			             (j == 6) ? 82  : 70;
			uint32_t col = 0xFF000000 | (v << 16) | (v << 8) | v;
			vram_fill_circle(px, py, dot_r, col);
		}

		vram_fence();

		// 60Hz-ish pacing using TSC (good enough for splash)
		target += step;
		while (rdtsc() < target) {
			__asm__ volatile("pause");
		}
	}
}

// ── Syscall globals ───────────────────────────────────────────────────────────
alignas(16) uint8_t g_syscall_kstack[64u * 1024u];
uint64_t g_saved_user_rsp = 0;

// ── klog globals ──────────────────────────────────────────────────────────────
#include "klog.h"
char     g_klog_buffer[KLOG_BUFFER_SIZE];
uint64_t g_klog_pos = 0;
bool     g_klog_bypass_framebuffer = false;
uint8_t* g_backbuffer = nullptr;
uint8_t* g_master_backbuffer = nullptr;

// Phase 11: Speed King globals
volatile uint32_t g_dirty_min_y = 0xFFFFFFFF;
volatile uint32_t g_dirty_max_y = 0;
volatile uint32_t g_dirty_min_x = 0xFFFFFFFF;
volatile uint32_t g_dirty_max_x = 0;

// Phase 9: WM post-flip hook (set to wm_render_chrome once WM is active)
post_flip_hook_fn g_post_flip_hook = nullptr;
pre_flip_hook_fn  g_pre_flip_hook  = nullptr;
static uint64_t last_mouse_flip_tsc = 0;

// Phase 9: Terminal window origin + size (set by wm_init)
int32_t g_term_ox = 8;          // fallback: full-screen margin
int32_t g_term_oy = 8;
uint32_t g_term_max_cols = 80;
uint32_t g_term_max_rows = 24;
uint32_t g_term_buf_height = 0;   // virtual backbuffer pixel height

int32_t g_mouse_x = 400;
int32_t g_mouse_y = 300;
uint8_t g_mouse_buttons = 0;
uint8_t g_mouse_prev_buttons = 0;
uint64_t tsc_hz = 3000000000ULL; // Calibrated at boot
volatile uint64_t g_idle_tsc_accum = 0;
volatile uint64_t g_tick_count = 0;
volatile bool g_needs_refresh = true;
volatile bool g_desktop_ready = false;
volatile bool wm_chrome_dirty = false;
volatile bool g_dragging_test = false;
volatile bool g_dragging_term = false;
volatile bool g_dragging_rain = false;
volatile bool g_dragging_log  = false;
volatile uint32_t g_mouse_btns_prev = 0;

#include "mouse.h"
#include "synaptics.h"
#include "ps2kb.h"

// Phase 18: High-precision TSC calibration against PIT
static void calibrate_tsc_pit() {
    print((char*)"Calibrating TSC...");
    
    // Wait for a PIT tick boundary (g_tick_count from timer.h)
    uint64_t start_tick = g_tick_count;
    while (g_tick_count == start_tick);
    
    // Start measurement
    uint64_t tsc1 = rdtsc();
    uint64_t m_start = g_tick_count;
    
    // Measuring for 100ms (100 ticks) for high precision
    while (g_tick_count < m_start + 100);
    uint64_t tsc2 = rdtsc();
    
    uint64_t delta = tsc2 - tsc1;
    tsc_hz = delta * 10; // 100ms * 10 = 1 second
    
    char mstr[32];
    print((char*)" TSC Hz: "); to_str(tsc_hz, mstr); print(mstr);
}

// ── PAT / Write-Combining Setup ──────────────────────────────────────────────
static void setup_pat() {
    uint64_t pat = rdmsr(0x277);
    // Entry 2 (PWT=0, PCD=1, PAT=0) -> value 0x01 (Write-Combining)
    pat &= ~(0xFFULL << 16);
    pat |=  (0x01ULL << 16);
    wrmsr(0x277, pat);
    print((char*)"[init] Framebuffer PAT Index 2 set to WC.");
}

uint64_t g_last_frame_ticks = 0;

extern "C" void mouse_process_scroll(int8_t scroll_dy) {
    if (!fb) return;
    int32_t mx = g_mouse_x, my = g_mouse_y;
    int idx = wm_find_top_window(mx, my);
    if (idx == WIN_TERM) {
        term_scroll_view(scroll_dy);
    } else if (idx == WIN_LOG) {
        wm_log_scroll(scroll_dy);
    }
}

extern "C" void mouse_process_input(int16_t dx, int16_t dy, uint8_t buttons) {
    if (!fb) return;

    g_mouse_prev_buttons = (uint8_t)g_mouse_buttons;
    g_mouse_buttons      = buttons;
    bool btn_held     = (g_mouse_buttons  & 1);
    bool btn_was_held = (g_mouse_prev_buttons & 1);
    bool mouse_press  = btn_held && !btn_was_held;
    bool mouse_release= !btn_held && btn_was_held;

    // 1. Move cursor (clamped to screen)
    g_mouse_x += dx;
    g_mouse_y += dy;
    if (dx != 0 || dy != 0 || buttons != g_mouse_prev_buttons) g_needs_refresh = true;

    if (g_mouse_x < 0) g_mouse_x = 0;
    if (g_mouse_x >= (int32_t)fb->width)  g_mouse_x = fb->width  - 1;
    if (g_mouse_y < 0) g_mouse_y = 0;
    if (g_mouse_y >= (int32_t)fb->height) g_mouse_y = fb->height - 1;

    // 2. Start drag / Raise / Close / Taskbar: hit-test Z-stack on fresh click
    if (mouse_press) {
        int32_t mx = g_mouse_x, my = g_mouse_y;
        
        // Taskbar / Start button
        if (my >= (int32_t)fb->height - (int32_t)g_taskbar_h) {
            if (mx >= 6 && mx <= (int32_t)(6 + g_sb_w)) {
                // Start button
                term_clear_screen();
                term_write_string((char*)"[SYSTEM] Start Menu coming soon...\r\n");
            } else {
                // Task buttons — mirror the draw layout exactly
                const int32_t WBTN_GAP = 4;
                int32_t cur_x = 6 + g_sb_w + 10;  // sb_x + g_sb_w + gap
                for (int idx = 0; idx < 4; idx++) {
                    if (!g_win_visible[idx]) { continue; }
                    if (mx >= cur_x && mx < cur_x + (int32_t)g_wbtn_w) {
                        wm_raise_window(idx);
                        break;
                    }
                    cur_x += g_wbtn_w + WBTN_GAP;
                }
            }
        }
 else {
            int idx = wm_find_top_window(mx, my);
            if (idx != -1) {
                wm_raise_window(idx); // Move to top
                if (wm_is_in_close(idx, mx, my)) {
                    wm_close_window(idx);
                } else if (wm_is_in_title(idx, mx, my)) {
                    // Start dragging the topmost window
                    if (idx == WIN_TERM) {
                        wm_ghost_dragging_term = true; g_dragging_term = true;
                        wm_drag_term_offx = mx - (int32_t)g_term_ox;
                        wm_drag_term_offy = my - (int32_t)g_term_oy;
                    } else if (idx == WIN_TEST) {
                        wm_ghost_dragging_test = true; g_dragging_test = true;
                        wm_drag_test_offx = mx - test_x;
                        wm_drag_test_offy = my - test_y;
                    } else if (idx == WIN_RAIN) {
                        wm_ghost_dragging_rain = true; g_dragging_rain = true;
                        wm_drag_rain_offx = mx - rain_x;
                        wm_drag_rain_offy = my - rain_y;
                    } else if (idx == WIN_LOG) {
                        wm_ghost_dragging_log = true; g_dragging_log = true;
                        wm_drag_log_offx = mx - log_win_x;
                        wm_drag_log_offy = my - log_win_y;
                    }
                }
            }
        }
    }

    // 3. Update ghost position while dragging (zero cost — just two int adds)
    if (wm_ghost_dragging_term) {
        wm_ghost_term_ox = g_mouse_x - wm_drag_term_offx;
        wm_ghost_term_oy = g_mouse_y - wm_drag_term_offy;
    }
    if (wm_ghost_dragging_test) {
        wm_ghost_test_ox = g_mouse_x - wm_drag_test_offx;
        wm_ghost_test_oy = g_mouse_y - wm_drag_test_offy;
    }
    if (wm_ghost_dragging_rain) {
        wm_ghost_rain_ox = g_mouse_x - wm_drag_rain_offx;
        wm_ghost_rain_oy = g_mouse_y - wm_drag_rain_offy;
    }
    if (wm_ghost_dragging_log) {
        wm_ghost_log_ox = g_mouse_x - wm_drag_log_offx;
        wm_ghost_log_oy = g_mouse_y - wm_drag_log_offy;
    }

    // 4. Commit on release: NOW the window actually moves, triggering one recompose
    if (mouse_release) {
        if (wm_ghost_dragging_term) {
            g_term_ox = wm_ghost_term_ox;
            g_term_oy = wm_ghost_term_oy;
            wm_ghost_dragging_term = false;
            g_dragging_term        = false;
            // Full screen dirty: old location + new location both need repaint
            g_dirty_min_x = 0; g_dirty_max_x = fb->width  - 1;
            g_dirty_min_y = 0; g_dirty_max_y = fb->height - 1;
            g_needs_refresh = true;
        }
        if (wm_ghost_dragging_test) {
            test_x = wm_ghost_test_ox;
            test_y = wm_ghost_test_oy;
            wm_ghost_dragging_test = false;
            g_dragging_test        = false;
            g_dirty_min_x = 0; g_dirty_max_x = fb->width  - 1;
            g_dirty_min_y = 0; g_dirty_max_y = fb->height - 1;
            g_needs_refresh = true;
        }
        if (wm_ghost_dragging_rain) {
            rain_x = wm_ghost_rain_ox;
            rain_y = wm_ghost_rain_oy;
            wm_ghost_dragging_rain = false;
            g_dragging_rain        = false;
            g_dirty_min_x = 0; g_dirty_max_x = fb->width  - 1;
            g_dirty_min_y = 0; g_dirty_max_y = fb->height - 1;
            g_needs_refresh = true;
        }
        if (wm_ghost_dragging_log) {
            log_win_x = wm_ghost_log_ox;
            log_win_y = wm_ghost_log_oy;
            wm_ghost_dragging_log = false;
            g_dragging_log        = false;
            g_dirty_min_x = 0; g_dirty_max_x = fb->width  - 1;
            g_dirty_min_y = 0; g_dirty_max_y = fb->height - 1;
            g_needs_refresh = true;
        }
    }
}

void compositor_thread_main() {
    uint64_t target_tsc = rdtsc(); // Start cadence from current time
    static uint64_t last_hud_tick     = 0;
    static uint32_t boot_sync_frames = 30;

    while (true) {
        uint64_t frame_interval = (tsc_hz / 60);
        target_tsc += frame_interval;

        // Run one frame of compositor logic
        uint64_t now_refresh = rdtsc();
        
        // If rainbow window is visible, always force a refresh for animation
        if (g_win_visible[WIN_RAIN]) {
            g_needs_refresh = true;
        }

        // Periodic HUD update: ensure Core0 stats refresh every 1 second
        if (now_refresh - last_hud_tick > tsc_hz) {
            last_hud_tick = now_refresh;
            g_needs_refresh = true;

            // Explicitly 'dirty' the HUD area (top-right strip)
            uint32_t hud_left = fb->width > 400 ? fb->width - 400 : 0;
            if (hud_left < g_dirty_min_x) g_dirty_min_x = hud_left;
            if (fb->width > g_dirty_max_x) g_dirty_max_x = fb->width - 1;
            if (0 < g_dirty_min_y) g_dirty_min_y = 0;
            if (60 > g_dirty_max_y) g_dirty_max_y = 60;
        }

        if (g_needs_refresh || boot_sync_frames > 0) {
            g_needs_refresh  = false;

            // Drain and render any pending terminal output
            term_process_ring_buffer();

            // Drive rainbow animation: 4 pixels per frame (rock solid now)
            if (g_win_visible[WIN_RAIN]) {
                rain_scroll += 4;
                // Mark rainbow area as dirty
                extern int32_t rain_x, rain_y, rain_w, rain_h;
                uint32_t rx1 = (uint32_t)rain_x, rx2 = (uint32_t)(rain_x + rain_w);
                uint32_t ry1 = (uint32_t)(rain_y - 28), ry2 = (uint32_t)(rain_y + rain_h);
                if (rx1 < g_dirty_min_x) g_dirty_min_x = rx1;
                if (rx2 > g_dirty_max_x) g_dirty_max_x = rx2;
                if (ry1 < g_dirty_min_y) g_dirty_min_y = ry1;
                if (ry2 > g_dirty_max_y) g_dirty_max_y = ry2;
            }
            bool animation_active = g_win_visible[WIN_RAIN]; 

            __asm__ volatile("cli");
            uint32_t fx1 = g_dirty_min_x, fx2 = g_dirty_max_x;
            uint32_t fy1 = g_dirty_min_y, fy2 = g_dirty_max_y;
            g_dirty_min_y = fb->height; g_dirty_max_y = 0;
            g_dirty_min_x = fb->width;  g_dirty_max_x = 0;
            __asm__ volatile("sti");

            if (boot_sync_frames > 0) {
                fx1 = 0; fx2 = fb->width - 1;
                fy1 = 0; fy2 = fb->height - 1;
                boot_sync_frames--;
            }
            
            // If animating, only dirty the rainbow window region to save bandwidth.
            if (animation_active) {
                uint32_t rx1 = rain_x > 0 ? (uint32_t)rain_x : 0;
                uint32_t rx2 = (uint32_t)(rain_x + rain_w);
                int32_t  ry1_s = rain_y - 28;
                uint32_t ry1 = ry1_s > 0 ? (uint32_t)ry1_s : 0;
                uint32_t ry2 = (uint32_t)(rain_y + rain_h);
                
                if (rx1 < fx1) fx1 = rx1;
                if (rx2 > fx2) fx2 = rx2;
                if (ry1 < fy1) fy1 = ry1;
                if (ry2 > fy2) fy2 = ry2;
            }

            uint64_t t_start = rdtsc();
            wm_compose_dirty(fx1, fx2, fy1, fy2);
            uint64_t t_end = rdtsc();
            g_last_frame_ticks = t_end - t_start;

			// signal the splash to exit after the first frame lands
            if (!g_desktop_ready) g_desktop_ready = true;
            
            term_dirty_reset();
        }

        // --- DRIFT-FREE SLEEP ---
        uint64_t final_now = rdtsc();
        if (final_now < target_tsc) {
            uint64_t diff = target_tsc - final_now;
            uint32_t ms_to_sleep = (uint32_t)(diff * 1000 / tsc_hz);
            if (ms_to_sleep > 0) {
                task_sleep_ms(ms_to_sleep);
            } else {
                yield(); // Too close for ms sleep; just yield the rest of the tick
            }
        } else {
            // Work took longer than 16.6ms (frame miss). 
            // Reset target to catch up and prevent "spiral of death"
            target_tsc = final_now;
            yield();
        }
    }
}

extern "C" void kmain(void) {
	if (!LIMINE_BASE_REVISION_SUPPORTED) hcf();
	if (!fb_req.response || fb_req.response->framebuffer_count < 1) hcf();

	fb = fb_req.response->framebuffers[0];

    // --- SSE & SIMD Enablement (CR0, CR4) ---
    {
        uint64_t cr0;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        cr0 &= ~(1ULL << 2); // Clear EM (Emulation)
        cr0 |= (1ULL << 1);  // Set MP (Monitor Coprocessor)
        __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");

        uint64_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1ULL << 9);  // Set OSFXSR (FXSAVE/FXRSTOR support)
        cr4 |= (1ULL << 10); // Set OSXMMEXCPT (SIMD exception support)
        __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
    }

	// ── NEW ──────────────────────────────────────
	init_gdt();
	syscall_init();
	print((char*)"GDT loaded.");
	init_idt();
	init_irq();              // remap PIC, mask all IRQ lines
	init_timer(1000);        // PIT at 1000 Hz → 1 ms tick


	__asm__ volatile ("sti"); // enable hardware interrupts
	calibrate_tsc_pit();
	synaptics_init();
	ps2kb_init();
	print((char*)"Interrupts enabled. Timer running.");
	// ── END NEW ──────────────────────────────────────

	print((char*)"Stack size req response:");
	if (stack_size_req.response == NULL) {
		print((char*)"NULL - stack NOT increased!");
	} else {
		print((char*)"OK - stack increased");
	}

	if (!memmap_req.response) {
		print((char*)"Error: No memmap response");
		hcf();
	}

	if (!hhdm_req.response) {
		print((char*)"Error: No hhdm response");
		hcf();
	}

	if (stack_size_req.response == NULL) {
		print((char*)"Error: stack size not increased");
		hcf();
	}

	HHDM = hhdm_req.response->offset;

	// Disable SMEP and SMAP so kernel can access user-mapped memory.
	{
		uint64_t cr4;
		__asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
		cr4 &= ~(1ULL << 20); // Clear SMEP
		cr4 &= ~(1ULL << 21); // Clear SMAP
		__asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
	}

	print((char*)"Setting up memory management");
	init_physical_allocator();
	map_hhdm_usable(PRESENT | WRITABLE);
	setup_pat();
	
	// Explicitly map the framebuffer in our new page tables if it's not already covered
	// by the usable RAM map. This ensures sys_write works from user tasks.
	if (fb_req.response && fb_req.response->framebuffer_count > 0) {
		struct limine_framebuffer *f = fb_req.response->framebuffers[0];
		uint64_t phys = (uint64_t)f->address - HHDM;
		uint64_t size = f->pitch * f->height;
		// Map it into the HHDM range explicitly with Write-Combining (WC) enabled via PCD bit.
		// PCD (bit 4) selects PAT entry that is typically WC on modern x86.
		uint64_t flags = PRESENT | WRITABLE | (1ULL << 4); 
		for (uint64_t offset = 0; offset < size; offset += 4096) {
			map_page((uint64_t)f->address + offset, phys + offset, flags);
		}
	}

	init_heap();
    
    // Allocate backbuffer for double buffering + huge scrollback history
    if (fb_req.response && fb_req.response->framebuffer_count > 0) {
        fb = fb_req.response->framebuffers[0];
        
        // 2000 lines @ 10px each = 20,000 pixels high
        // Optimized backbuffer size: 1000px height is plenty for our 600px window
        g_term_buf_height = 1000;
        if (g_term_buf_height < fb->height) g_term_buf_height = fb->height;
        g_backbuffer = (uint8_t*)kmalloc(g_term_buf_height * fb->pitch);
        if (g_backbuffer) {
            memset(g_backbuffer, 0, g_term_buf_height * fb->pitch);
        }

        g_master_backbuffer = (uint8_t*)kmalloc(fb->height * fb->pitch);
        if (g_master_backbuffer) {
            memset(g_master_backbuffer, 0, fb->height * fb->pitch);
            
            g_needs_refresh = true;
        }
    }

	ramfs_init();
	ramfs_mount_klog();

    // Optional wallpaper module: if a second Limine module is present,
    // treat it as a raw BGRA wallpaper and also expose it as /wallpaper.raw.
    if (module_req.response && module_req.response->module_count > 1) {
        struct limine_file* wmod  = module_req.response->modules[1];
        uint64_t            wsize = wmod->size;
        const uint8_t*      wdata = (const uint8_t*)wmod->address;

        // Mirror into ramfs so userspace can read it as /wallpaper.raw.
        file wf;
        if (vfs_open("/wallpaper.raw", 0, &wf) == 0) {
            vfs_write(&wf, wdata, wsize);
            vfs_close(&wf);
        }

        // Feed raw pixels directly into the WM wallpaper buffer.
        wm_load_wallpaper_from_raw(wdata, wsize);
    }

	scheduler_init();
    // Initializing graphical UI components
    wm_init();     // Initialize window manager and UI scaling first
    wm_log_init(); // Then initialize the log buffer with the correct scaled dimensions
    
    // Spawn compositor thread extremely early so it guarantees 60fps
    // screen refreshes regardless of whether USB enumeration blocks.
    scheduler_add_task(task_create(compositor_thread_main));

	boot_loading_screen_until_ready();

    if (smp_req.response) {
        char cstr[32];
        print((char*)"[cpu] Detected cores: ");
        to_str(smp_req.response->cpu_count, cstr);
        print(cstr); print((char*)"\r\n");
    }

	if (!rsdp_req.response) hcf();
	uint64_t rsdp_va = (uint64_t)rsdp_req.response->address;
	volatile uint8_t *ptr = (volatile uint8_t*)rsdp_va;

	const char expected[8] = {'R','S','D',' ','P','T','R',' '};
	bool valid_signature = true;
	for (int i = 0; i < 8; i++) {
		if (ptr[i] != expected[i]) { valid_signature = false; break; }
	}
	if (!valid_signature) {
		print((char*)"Error: No valid signature for RSD ptr");
		hcf();
	}

	uint32_t rsdt_pa_32 =
		(uint32_t)ptr[0x10]         |
		((uint32_t)ptr[0x11] <<  8) |
		((uint32_t)ptr[0x12] << 16) |
		((uint32_t)ptr[0x13] << 24);

	uint64_t rsdt_va = HHDM + (uint64_t)rsdt_pa_32;
	volatile uint8_t *ptr_rsdt = (volatile uint8_t *)rsdt_va;

	const char expected_rsdt[4] = {'R','S','D','T'};
	valid_signature = true;
	for (int i = 0; i < 4; i++) {
		if (ptr_rsdt[i] != expected_rsdt[i]) { valid_signature = false; break; }
	}
	if (!valid_signature) {
		print((char*)"Error: No valid signature for rsdt");
		hcf();
	}

	uint32_t rsdt_length =
		(uint32_t)ptr_rsdt[4]         |
		((uint32_t)ptr_rsdt[5] <<  8) |
		((uint32_t)ptr_rsdt[6] << 16) |
		((uint32_t)ptr_rsdt[7] << 24);
	uint32_t entry_count = (rsdt_length - 36) / 4;

	volatile uint8_t *ptr_mcfg = 0;
	for (uint32_t i = 0; i < entry_count; i++) {
		uint32_t offset   = 36 + i * 4;
		uint32_t entry_pa =
			(uint32_t)ptr_rsdt[offset]         |
			((uint32_t)ptr_rsdt[offset+1] <<  8) |
			((uint32_t)ptr_rsdt[offset+2] << 16) |
			((uint32_t)ptr_rsdt[offset+3] << 24);
		uint64_t entry_va   = HHDM + (uint64_t)entry_pa;
		volatile uint8_t *e = (volatile uint8_t *)entry_va;
		if (e[0]=='M' && e[1]=='C' && e[2]=='F' && e[3]=='G') {
			ptr_mcfg = e;
			break;
		}
	}
	if (ptr_mcfg == 0) { print((char*)"Error: ptr_mcfg == 0"); hcf(); }

	volatile struct MCFGHeader *mcfg = (volatile struct MCFGHeader *)ptr_mcfg;
	uint32_t length  = mcfg->header.length;
	uint32_t entries = (length - sizeof(struct ACPISDTHeader) - 8) / 16;

	uint64_t usb_virt_base = 0;
	uint8_t  usb_start = 0, usb_bus = 0, usb_dev = 0, usb_fn = 0, usb_prog_if = 0;
	bool     usb_found = false;

	for (uint32_t i = 0; i < entries; i++) {
		auto *e       = &mcfg->entries[i];
		uint64_t phys_base = e->base_address;
		uint16_t seg       = e->pci_segment_group;
		uint8_t  start     = e->start_bus;
		uint8_t  end       = e->end_bus;

		print((char*)"Mapping ECAM");
		uint64_t ecam_size = (uint64_t)(end - start + 1) * 0x100000ULL;
		uint64_t virt_base = PCI_ECAM_VA_BASE + (uint64_t)seg * PCI_ECAM_SEG_STRIDE;
		map_ecam_region(phys_base, virt_base, ecam_size);
		print((char*)"Mapped...");

		for (uint16_t bus = start; bus <= end; bus++) {
			for (uint8_t dev = 0; dev < 32; dev++) {
				uint32_t id0 = pci_cfg_read32(virt_base, start, bus, dev, 0, 0x00);
				if (id0 == 0xFFFFFFFF) continue;
				
				uint8_t header_type = (pci_cfg_read32(virt_base, start, bus, dev, 0, 0x0C) >> 16) & 0xFF;
				int max_fn = (header_type & 0x80) ? 8 : 1;

				for (uint8_t fn = 0; fn < max_fn; fn++) {
					uint32_t id = pci_cfg_read32(virt_base, start, bus, dev, fn, 0x00);
					if (id == 0xFFFFFFFF) continue;
					uint32_t class_reg = pci_cfg_read32(virt_base, start, bus, dev, fn, 0x08);
					if (((class_reg >> 24) & 0xFF) == 0x0C &&
					    ((class_reg >> 16) & 0xFF) == 0x03) {
						
						uint8_t prog_if = (class_reg >> 8) & 0xFF;
						if (prog_if == 0x30) {
						    if (xhci_count < MAX_CONTROLLERS) {
							    xhci_hc[xhci_count].virt_base = virt_base;
							    xhci_hc[xhci_count].bus = bus;
							    xhci_hc[xhci_count].dev = dev;
							    xhci_hc[xhci_count].fn = fn;
							    xhci_count++;
							    print((char*)"Found an xHCI controller!");
							} else {
							    print((char*)"Found xHCI controller but array is full, skipping...");
							}
						} else {
							print((char*)"Found non-xHCI USB controller, skipping...");
						}
					}
				}
			}
		}
	}

	if (xhci_count == 0) { print((char*)"Could not find any xHCI USB controller!"); hcf(); }

    for (int c = 0; c < xhci_count; c++) {
        xhci_hc[c].needsResetting = true;
    }

	char str[64];

	// ── Outer restart loop ───────────────────────────────────────────────────
	// When the main loop detects a disconnect or Port Status Change, it sets
	// needsResetting=true and breaks.  We jump back here, re-run setupUSB to
	// re-enumerate all ports, reinitialise per-keyboard state, re-queue TRBs,
	// and re-enter the main loop.
	restart:
    for (int c = 0; c < xhci_count; c++) {
        if (xhci_hc[c].portfailed) {
            for (int _p = 0; _p < 4096; _p++) xhci_hc[c].portfailed[_p] = 0;
        }
    }
    
	bool any_reset = true;
	while (any_reset) {
        any_reset = false;
        for (int c = 0; c < xhci_count; c++) {
            if (xhci_hc[c].needsResetting) {
                setupUSB(&xhci_hc[c]);
                if (xhci_hc[c].needsResetting) any_reset = true;
            }
        }
    }

	// ── Queue hub status-change TRBs BEFORE the keyboard_count == 0 check ───
	//
	// CRITICAL ORDER: hub TRBs must be queued here, before the empty-keyboard
	// wait loop below, not after it.
	//
	// The old code queued hub TRBs after the keyboard TRB loop, which is only
	// reached when keyboard_count > 0.  When an empty hub is plugged in,
	// keyboard_count == 0 so the wait loop fires first — hub TRBs are never
	// queued, the interrupt IN pipe is never armed, and the hub is completely
	// invisible.  It cannot signal downstream connect, disconnect, or anything
	// else, because there is no pending TRB for the xHCI to complete.
	//
	// With hub TRBs queued here:
	//   - Empty hub plug/unplug → Transfer Event on hub slot → wait loop
	//     catches it → goto restart → keyboard found on next pass.
	//   - Hub with keyboard already attached → same path, works identically.
	//   - No hub at all → hub_count == 0, loop body never runs, no cost.
	for (int h = 0; h < hub_count; h++) {
		USBDevice* hd = hubs[h];

		volatile uint8_t* buf = (volatile uint8_t*)alloc_table();
		for (int j = 0; j < 4096; j++) buf[j] = 0;
		uint64_t buf_phys = (uint64_t)buf - HHDM;

		volatile TRB* trb = &hd->hub_ring->trb[hd->hub_ring->enq];
		trb->parameter = buf_phys;
		trb->status    = 1;   // 1 byte status-change bitmap (covers up to 7 ports)
		trb->control   = (1u << 10) | (1u << 5) | (hd->hub_ring->pcs & 1u);

		if (++hd->hub_ring->enq == LINK_INDEX) {
			hd->hub_ring->trb[LINK_INDEX].control =
				(TRB_TYPE_LINK << 10) | (1u << 1) | (hd->hub_ring->pcs & 1u);
			hd->hub_ring->enq = 0;
			hd->hub_ring->pcs ^= 1;
		}

		hd->hc->doorbell32[hd->slot_id] = hub_dcis[h];
		print((char*)"Queued status-change TRB for hub slot:");
		to_str(hd->slot_id, str); print(str);
	}

	print((char*)"[init] Decoupled terminal boot.");
	static bool init_started = false;
	if (!init_started && module_req.response && module_req.response->module_count > 0) {
		struct limine_file *module = module_req.response->modules[0];
		Task* init_task = execve_memory((const uint8_t*)module->address);
		if (init_task) {
			// Phase 9: register post-flip chrome hook
			g_pre_flip_hook = nullptr; // Phase 18: handled by wm_compose_full
			g_post_flip_hook = nullptr;

			// Re-enable the shell task — it now runs "inside" the WM window
			scheduler_add_task(init_task);
			init_started = true;
			g_klog_bypass_framebuffer = true;
			print((char*)"[wm] Shell task started inside WM frame.");
		} else {
			print((char*)"[init] Failed to load ELF module.");
		}
	}

	// ── Wait for a keyboard ONLY if we need to enumerate first pass ─────────
	// We no longer block indefinitely here. The main loop will handle hotplug.
	if (keyboard_count == 0) {
		print((char*)"No keyboard found yet. Proceeding to background polling...");
	}

	// ── TSC-based repeat thresholds ───────────────────────────────────────────
	//
	// Linux defaults (same values used in drivers/tty/vt/keyboard.c):
	//   repeat_delay  = 250 ms
	//   repeat_period = 40  ms  (25 repeats / sec)
	//
	// We convert to TSC ticks so timing is correct regardless of loop speed.
	// tsc_hz is already calibrated via calibrate_tsc_pit() earlier in kmain.

	// Print so you can verify CPUID gave a sensible value.
	print((char*)"TSC freq (Hz):"); to_str(tsc_hz, str); print(str);

	const uint64_t REPEAT_DELAY  = tsc_hz * 250 / 1000;   // 250 ms in ticks
	const uint64_t REPEAT_PERIOD = tsc_hz *  40 / 1000;   //  40 ms in ticks

	// ── Per-keyboard state ────────────────────────────────────────────────────

	uint8_t  last_keys[MAX_KEYBOARDS][6];
	uint8_t  last_mods[MAX_KEYBOARDS];
	for (int k = 0; k < MAX_KEYBOARDS; k++) {
		for (int j = 0; j < 6; j++) last_keys[k][j] = 0;
		last_mods[k] = 0;
	}

	// key_down_at[k][keycode]      = TSC tick when key first went down, 0 if up.
	// last_repeat_fire[k][keycode] = TSC tick of last synthetic repeat emission.
	uint64_t key_down_at[MAX_KEYBOARDS][256];
	uint64_t last_repeat_fire[MAX_KEYBOARDS][256];
	for (int k = 0; k < MAX_KEYBOARDS; k++)
		for (int i = 0; i < 256; i++) {
			key_down_at[k][i]      = 0;
			last_repeat_fire[k][i] = 0;
		}

	print((char*)"About to enter main loop");

	// ── Drain stale events accumulated during enumeration ────────────────────
	// Non-keyboard devices (e.g. a USB boot drive) may have left pending
	// Transfer Events in the ring from their EP0 descriptor reads, or from
	// being left in a confused state after UEFI used them.  Consume and discard
	// everything now so the main loop starts with a clean event ring.
	// We do this BEFORE queueing keyboard TRBs so we can't accidentally
	// discard a real event.
	{
        for (int c = 0; c < xhci_count; c++) {
		    USB_Response stale;
		    do { stale = get_usb_response(&xhci_hc[c], 1); } while (stale.gotresponse);
		}
	}

	for (int k = 0; k < keyboard_count; k++) {
		USBDevice* kd = keyboards[k];
		for (int i = 0; i < kd->interface_count; i++) {
			HIDInterface* hi = &kd->interfaces[i];
			if (hi->type != 1) continue;

			volatile uint8_t* buf = (volatile uint8_t*)alloc_table();
			for (int j = 0; j < 4096; j++) buf[j] = 0;
			uint64_t buf_phys = (uint64_t)buf - HHDM;

			volatile TRB* trb = &hi->ring->trb[hi->ring->enq];
			trb->parameter = buf_phys;
			trb->status    = (uint32_t)hi->mps;
			trb->control   = (1u << 10) | (1u << 5) | (hi->ring->pcs & 1u);

			if (++hi->ring->enq == LINK_INDEX) {
				hi->ring->trb[LINK_INDEX].control =
					(TRB_TYPE_LINK << 10) | (1u << 1) | (hi->ring->pcs & 1u);
				hi->ring->enq = 0;
				hi->ring->pcs ^= 1;
			}

			uint8_t kbd_dci = hi->ep_num * 2 + 1;
			kd->hc->doorbell32[kd->slot_id] = kbd_dci;
		}
	}

	// ── Queue one initial TRB per mouse ──────────────────────────────────────
	for (int m = 0; m < mouse_count; m++) {
		USBDevice* md = mice[m];
		for (int i = 0; i < md->interface_count; i++) {
			HIDInterface* hi = &md->interfaces[i];
			if (hi->type != 2) continue;

			volatile uint8_t* buf = (volatile uint8_t*)alloc_table();
			for (int j = 0; j < 4096; j++) buf[j] = 0;
			uint64_t buf_phys = (uint64_t)buf - HHDM;

			volatile TRB* trb = &hi->ring->trb[hi->ring->enq];
			trb->parameter = buf_phys;
			trb->status    = (uint32_t)hi->mps;
			trb->control   = (1u << 10) | (1u << 5) | (hi->ring->pcs & 1u);

			if (++hi->ring->enq == LINK_INDEX) {
				hi->ring->trb[LINK_INDEX].control =
					(TRB_TYPE_LINK << 10) | (1u << 1) | (hi->ring->pcs & 1u);
				hi->ring->enq = 0;
				hi->ring->pcs ^= 1;
			}

			uint8_t mouse_dci = hi->ep_num * 2 + 1;
			md->hc->doorbell32[md->slot_id] = mouse_dci;
		}
	}

	// NOTE: Hub TRBs were already queued before the keyboard_count == 0 block
	// above.  Do NOT queue them again here — doing so would advance enq/pcs
	// and ring the doorbell on an already-armed pipe, producing a duplicate
	// or corrupt TRB that confuses the xHCI.

	print((char*)"Press any key...");

	// Terminal task is now started early, above the repeat threshold calculation.
	// This ensures it survives disconnect/restart cycles.

	uint32_t event_count = 0;

	// --- PHASE 18.5: GUARANTEED FULL-SCREEN SYNC ---
	// Before we enter the reactive loop, ensure the VERY FIRST frame captures the whole screen.
	g_dirty_min_x = 0;
	g_dirty_max_x = fb->width - 1;
	g_dirty_min_y = 0;
	g_dirty_max_y = fb->height - 1;
	g_needs_refresh = true;

	while (true) {
		event_count = 0;

		// ── Drain ALL pending USB events to handle 1000Hz HID traffic ─────────
		while (true) {
            bool any_resp = false;
            for (int ctrl_idx = 0; ctrl_idx < xhci_count; ctrl_idx++) {
                if (xhci_hc[ctrl_idx].needsResetting) continue;
			    // 0ms timeout: fast-poll the event ring without busy-waiting.
			    USB_Response resp = get_usb_response(&xhci_hc[ctrl_idx], 0);
			    if (!resp.gotresponse) continue;

                any_resp = true;
			    event_count++;

			// ── Port Status Change (type 34) ─────────────────────────────────
			// A PSC event means something changed on a root port — a device was
			// connected, disconnected, or reset.  Trigger a full re-enumeration.
			// 150ms settle delay gives PORTSC time to stabilise on same-port
			// reconnects before setupUSB scans it.
			if (resp.type == 34) {
				uint32_t port_id = (resp.event->parameter >> 24) & 0xFF;
				print((char*)"PSC event on port:");
				to_str(port_id, str); print(str);
				tsc_delay_ms(150);
				xhci_hc[ctrl_idx].needsResetting = true;
				goto restart;
			}

			if (resp.type == 32) {   // Transfer Event
				uint32_t code    = (resp.event->status >> 24) & 0xFF;
				uint64_t trb_ptr = resp.event->parameter;
				uint32_t slot_from_event = (resp.ctrl >> 24) & 0xFF;

				// ── Hub status-change interrupt ───────────────────────────────
				//
				// When any downstream port changes state (connect, disconnect,
				// reset complete), the hub fires its interrupt IN endpoint.
				// We get a Transfer Event here on the hub's slot.  Like Linux
				// hub_irq() → schedule hub_event() → hub_port_status(), we
				// restart full enumeration so every changed port is handled.
				//
				// 300ms settle delay before restart so PORTSC is stable on
				// same-port reconnects.
				{
					int which_hub = -1;
					for (int h = 0; h < hub_count; h++) {
						if (hubs[h]->slot_id == slot_from_event && hubs[h]->hc == &xhci_hc[ctrl_idx]) { which_hub = h; break; }
					}

					if (which_hub >= 0 && (code == 1 || code == 13)) {
						print((char*)"Hub status-change interrupt — re-enumerating.");

						// Re-queue the status-change TRB so the pipe stays armed.
						USBDevice* hd     = hubs[which_hub];
						volatile TRB* nxt = &hd->hub_ring->trb[hd->hub_ring->enq];
						nxt->parameter = ((volatile TRB*)(HHDM + trb_ptr))->parameter;
						nxt->status    = 1;
						nxt->control   = (1u << 10) | (1u << 5) | (hd->hub_ring->pcs & 1u);
						if (++hd->hub_ring->enq == LINK_INDEX) {
							hd->hub_ring->trb[LINK_INDEX].control =
								(TRB_TYPE_LINK << 10) | (1u << 1) | (hd->hub_ring->pcs & 1u);
							hd->hub_ring->enq = 0;
							hd->hub_ring->pcs ^= 1;
						}
						hd->hc->doorbell32[hd->slot_id] = hub_dcis[which_hub];

						tsc_delay_ms(150);
						xhci_hc[ctrl_idx].needsResetting = true;
						goto restart;
					}

					// Transfer error on a hub slot → hub itself disconnected.
					if (which_hub >= 0 && code != 1 && code != 13) {
						print((char*)"Hub transfer error, code:");
						to_str(code, str); print(str);
						tsc_delay_ms(150);
						xhci_hc[ctrl_idx].needsResetting = true;
						goto restart;
					}
				}

				if (code == 1 || code == 13) {   // Success or Short Packet

					// trb_ptr is the physical address of the completed TRB.
					// Dereference it to get the actual HID report buffer address.
					volatile TRB*     completed_trb = (volatile TRB*)(HHDM + trb_ptr);
					uint64_t          report_phys   = completed_trb->parameter;
					volatile uint8_t* report        = (volatile uint8_t*)(HHDM + report_phys);

					uint32_t dci_from_event = (resp.ctrl >> 16) & 0x1F;

					int which_kbd = -1;
					HIDInterface* k_hi = nullptr;
					for (int k = 0; k < keyboard_count; k++) {
						if (keyboards[k]->slot_id == slot_from_event && keyboards[k]->hc == &xhci_hc[ctrl_idx]) {
							for (int i=0; i<keyboards[k]->interface_count; i++) {
								if (keyboards[k]->interfaces[i].type == 1 && 
									(keyboards[k]->interfaces[i].ep_num*2+1) == dci_from_event) {
									which_kbd = k;
									k_hi = &keyboards[k]->interfaces[i];
									break;
								}
							}
						}
						if (which_kbd >= 0) break;
					}

					int which_mouse = -1;
					HIDInterface* m_hi = nullptr;
					for (int m = 0; m < mouse_count; m++) {
						if (mice[m]->slot_id == slot_from_event && mice[m]->hc == &xhci_hc[ctrl_idx]) {
							for (int i=0; i<mice[m]->interface_count; i++) {
								if (mice[m]->interfaces[i].type == 2 && 
									(mice[m]->interfaces[i].ep_num*2+1) == dci_from_event) {
									which_mouse = m;
									m_hi = &mice[m]->interfaces[i];
									break;
								}
							}
						}
						if (which_mouse >= 0) break;
					}

					if (which_kbd >= 0) {
						int     k         = which_kbd;
						uint8_t modifiers = report[0];
						uint64_t now      = rdtsc();

						// Mark device as a dongle if it's sending keyboard traffic
						// (Razer dongles flood the interface during mouse movement).
						keyboards[k]->is_dongle = true;

						// ── Mark released keys ────────────────────────────────
						for (int j = 0; j < 6; j++) {
							uint8_t old = last_keys[k][j];
							if (old == 0) continue;
							bool still_held = false;
							for (int b = 2; b < 8; b++) {
								if (report[b] == old) { still_held = true; break; }
							}
							if (!still_held) {
								key_down_at[k][old]      = 0;
								last_repeat_fire[k][old] = 0;
							}
						}

						// ── Emit and timestamp newly-pressed keys ─────────────
						// Razer dongles (4 interfaces) send Battery/DPI data over 
						// the keyboard channel. We suppress terminal typing for them.
						if (keyboards[k]->interface_count != 4) {
							for (int b = 2; b < 8; b++) {
								uint8_t key = report[b];
							if (key == 0) continue;
							if (key_down_at[k][key] == 0) {
								// Fresh press: record TSC time and emit immediately.
								key_down_at[k][key] = now;
								bool shift = (modifiers & 0x22u) != 0;
								bool ctrl  = (modifiers & 0x11u) != 0;
								if (key >= 0x4F && key <= 0x52) {
									// Ctrl + Up/Down = Log window scroll
									if (ctrl && key == 0x52) wm_log_scroll(-1);
									else if (ctrl && key == 0x51) wm_log_scroll(1);
									// Shift + Up/Down = Terminal Scrollback
									else if (shift && key == 0x52) term_scroll_view(-1);
									else if (shift && key == 0x51) term_scroll_view(1);
									else {
										// Regular Arrow = Shell Navigation
										tty_input('\x1b'); tty_input('[');
										char arrow_codes[] = {'C', 'D', 'B', 'A'};
										tty_input(arrow_codes[key - 0x4F]);
									}
								} else {
									char c = shift ? hid_to_ascii_shift[key] : hid_to_ascii[key];
									if (c) tty_input(c);
								}
							}
						}
						}
						for (int j = 0; j < 6; j++) last_keys[k][j] = report[2 + j];
						last_mods[k] = modifiers;

						// ── Re-queue transfer TRB ──────────────────────────────
						volatile TRB* new_trb = &k_hi->ring->trb[k_hi->ring->enq];
						new_trb->parameter = report_phys;
						new_trb->status    = (uint32_t)k_hi->mps;
						new_trb->control   = (1u << 10) | (1u << 5) | (k_hi->ring->pcs & 1u);

						if (++k_hi->ring->enq == LINK_INDEX) {
							k_hi->ring->trb[LINK_INDEX].control =
								(TRB_TYPE_LINK << 10) | (1u << 1) | (k_hi->ring->pcs & 1u);
							k_hi->ring->enq = 0;
							k_hi->ring->pcs ^= 1;
						}
						keyboards[k]->hc->doorbell32[slot_from_event] = dci_from_event;
					} else if (which_mouse >= 0) {
						int m = which_mouse;

						// Report ID skip heuristic:
						// Protocol 0 (Report Protocol) always has a Report ID prefix byte.
						// Composite devices with small MPS may also use Report IDs.
						uint8_t* data = (uint8_t*)report;
						bool shifted = false;
						if (m_hi->protocol == 0 || (mice[m]->interface_count > 1 && m_hi->mps <= 8 && (report[0] == 0x01 || report[0] == 0x02))) {
							data++;
							shifted = true;
						}

						// Parse mouse report: Report Protocol mice (like wireless
						// gaming dongles) often use 16-bit LE X/Y deltas.
						// We use 16-bit parsing if Protocol != 2 (Boot) OR if MPS > 8 
						// (high-res gaming mice often claim Boot mode but send larger reports).
						{
							uint8_t buttons = data[0];
							int16_t dx, dy;
							int8_t scroll = 0;

							// Auto-switcher: Use extended 16-bit path if we established this is a Razer Dongle.
							// We match the Razer's exact physical USB profile (MPS=64, 4 Interfaces) 
							// plus the keyboard traffic fingerprint so we don't accidentally
							// apply this layout to standard composite trackpad keyboards.
							bool is_razer_wireless = (mice[m]->is_dongle && m_hi->mps == 64 && mice[m]->interface_count == 4);
							if (m_hi->protocol != 2 || is_razer_wireless) {
								if (m_hi->mps >= 8) {
									// Wireless Razer Dongle (Extended 16-bit layout)
									// [0] Buttons
									// [1/2] Padding
									// [3] Scroll
									// [4/5] X (16-bit LE)
									// [6/7] Y (16-bit LE)
									dx = (int16_t)((uint16_t)data[4] | ((uint16_t)data[5] << 8));
									dy = (int16_t)((uint16_t)data[6] | ((uint16_t)data[7] << 8));
									scroll = (int8_t)data[3];
								}
							} else {
								// Wired Razer Mouse / Standard Boot Protocol
								// [0] Buttons
								// [1] X (8-bit signed)
								// [2] Y (8-bit signed)
								// [3] Scroll (8-bit signed)
								dx = (int16_t)(int8_t)data[1];
								dy = (int16_t)(int8_t)data[2];
								if (m_hi->mps >= 4) scroll = (int8_t)data[3];
							}

							mouse_process_input(dx, dy, buttons);
							if (scroll != 0) mouse_process_scroll(-scroll);
						}

						// Re-queue transfer TRB
						volatile TRB* new_trb = &m_hi->ring->trb[m_hi->ring->enq];
						new_trb->parameter = report_phys;
						new_trb->status    = (uint32_t)m_hi->mps;
						new_trb->control   = (1u << 10) | (1u << 5) | (m_hi->ring->pcs & 1u);

						if (++m_hi->ring->enq == LINK_INDEX) {
							m_hi->ring->trb[LINK_INDEX].control =
								(TRB_TYPE_LINK << 10) | (1u << 1) | (m_hi->ring->pcs & 1u);
							m_hi->ring->enq = 0;
							m_hi->ring->pcs ^= 1;
						}
						mice[m]->hc->doorbell32[slot_from_event] = dci_from_event;
					} else {
						// ── Transfer error on a keyboard slot → device disconnected ──
						uint32_t err_slot = slot_from_event;
						for (int k = 0; k < keyboard_count; k++) {
							if (keyboards[k]->slot_id == err_slot && keyboards[k]->hc == &xhci_hc[ctrl_idx]) {
								print((char*)"Transfer error on keyboard slot, code:");
								to_str(code, str); print(str);
								// STABILITY FIX: We intentionally do NOT reset the controller here.
								// Transient transfer errors on keyboards should not sever the entire
								// USB subsystem. The port will remain active.
								break;
							}
						}
					}
				}
			}
        } // closes for loop
        if (!any_resp) break;
		}
		// Actual VRAM rendering is deferred to the 60 Hz compositor thread
		// to avoid exhausting PCIe transaction limits with 1000 Hz micro-writes.

		// ── Universal PORTSC Hotplug Polling (Software PSC) ───────────────────
		// The AMD B850 chipset drops hardware PSC interrupts for ports that are
		// disabled (PED=0) but still powered, completely missing device removals
		// and re-insertions. We manually poll the CCS bit of all root ports every
		// frame. If any port's physical connection state changes, we trigger a
		// controller reset to safely handle the hotplug.
        for (int ctrl_idx = 0; ctrl_idx < xhci_count; ctrl_idx++) {
            XHCIController* hc = &xhci_hc[ctrl_idx];
            if (hc->needsResetting) continue;
            
            uint32_t current_ccs = 0;
            for (uint8_t p = 0; p < hc->max_ports && p < 32; p++) {
                volatile uint32_t* portsc = (volatile uint32_t*)((uintptr_t)hc->ops + 0x400 + p * 0x10);
                if (*portsc & 1u) current_ccs |= (1u << p);
            }
            
            if (current_ccs != hc->last_ccs) {
                print((char*)"Root port CCS changed. Triggering software hotplug event.");
                hc->last_ccs = current_ccs;
                tsc_delay_ms(150);
                hc->needsResetting = true;
                goto restart;
            }
        }

		// ── Software Typematic Repeat ─────────────────────────────────────────────
		// USB keyboards do not repeat keys in hardware when SET_IDLE is configured
		// to 0. We implement standard 500ms delay + 30ms repeat rate in software.
		uint64_t current_tsc = rdtsc();
		uint64_t ms_to_tsc = _usb_tsc_hz() / 1000;
		uint64_t repeat_delay_tsc = 500 * ms_to_tsc;
		uint64_t repeat_rate_tsc  = 30 * ms_to_tsc;

		for (int k = 0; k < keyboard_count; k++) {
			if (keyboards[k]->interface_count == 4) continue; // Skip phantom dongles
			for (int key = 2; key < 256; key++) {
				uint64_t down_time = key_down_at[k][key];
				if (down_time != 0) {
					if ((current_tsc - down_time) > repeat_delay_tsc) {
						if (last_repeat_fire[k][key] == 0 || (current_tsc - last_repeat_fire[k][key]) > repeat_rate_tsc) {
							last_repeat_fire[k][key] = current_tsc;
							
							// Repeat key emission
							uint8_t modifiers = last_mods[k];
							bool shift = (modifiers & 0x22u) != 0;
							bool ctrl  = (modifiers & 0x11u) != 0;
							
							if (key >= 0x4F && key <= 0x52) {
								if (ctrl && key == 0x52) wm_log_scroll(-1);
								else if (ctrl && key == 0x51) wm_log_scroll(1);
								else if (shift && key == 0x52) term_scroll_view(-1);
								else if (shift && key == 0x51) term_scroll_view(1);
								else {
									tty_input('\x1b'); tty_input('[');
									char arrow_codes[] = {'C', 'D', 'B', 'A'};
									tty_input(arrow_codes[key - 0x4F]);
								}
							} else {
								char c = shift ? hid_to_ascii_shift[key] : hid_to_ascii[key];
								if (c) tty_input(c);
							}
						}
					}
				}
			}
		}

        // Standardize input polling to ~125Hz. This reduces scheduler thrashing
        // and gives the high-priority compositor more airtime during input-heavy
        // tasks like scrolling.
        task_sleep_ms(8);
	}
}

extern "C" void kpanic(const char* name, ExceptionFrame* f) {
    // 1. Silence the system
    __asm__ volatile("cli");

    // 2. Graphical BSOD: MyOS "Deep Space" theme
    uint32_t myos_panic_bg = 0xFF141414; // Ultra dark/black
    vram_fill(myos_panic_bg);

    // 3. Header
    int32_t x = 100;
    int32_t y = 100;

    vram_draw_char_8x8(x, y, ':', 0xFFFF0000);
    vram_draw_char_8x8(x + 8, y, '(', 0xFFFF0000);
    y += 40;

    vram_draw_string_8x8(x, y, "--- !!! MyOS CRITICAL SYSTEM FAILURE !!! ---", 0xFFFF0000); // Red alert
    y += 40;

    vram_draw_string_8x8(x, y, "The kernel has encountered an unrecoverable exception.", 0xFFFFFFFF);
    y += 20;
    vram_draw_string_8x8(x, y, "Execution was halted to protect the filesystem and hardware state.", 0xFFFFFFFF);
    y += 60;

    vram_draw_string_8x8(x, y, "FAULT_ID: ", 0xFFBBBBBB);
    vram_draw_string_8x8(x + 10*8, y, name, 0xFF00AAFF); // Cyan error name
    y += 40;

    // 4. Register State
    char buf[32];
    auto dump_reg = [&](const char* rname, uint64_t val) {
        vram_draw_string_8x8(x, y, rname, 0xFFAAAAAA);
        to_hex(val, buf);
        vram_draw_string_8x8(x + 9*8, y, buf, 0xFFFFFFFF);
        y += 12;
    };

    if (f) {
        dump_reg("INST_PTR:", f->rip);
        dump_reg("ERR_CODE:", f->error_code);
        dump_reg("STACK_P: ", f->rsp);
        dump_reg("RAX:     ", f->rax);
        dump_reg("RBX:     ", f->rbx);
        dump_reg("RCX:     ", f->rcx);
        dump_reg("RDX:     ", f->rdx);
        dump_reg("FLAGS:   ", f->rflags);
        
        uint64_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        if (f->vector == 14) dump_reg("V_ADDR:  ", cr2); // Faulting addr for Page Fault
    } else {
        vram_draw_string_8x8(x, y, "[ NO EXCEPTION FRAME ]", 0xFFFFAAAA);
    }

    y += 40;
    vram_draw_string_8x8(x, y, "SYSTEM HALTED. PLEASE RESTART MANUALLY.", 0xFFFF5555);

    vram_fence();
    hcf();
}