#ifndef WM_H
#define WM_H

#include <stdint.h>
#include <stddef.h>
#include "app.h"
#include "graphics.h"
#include "framebufferstuff.h"
#include "heap.h"
#include "mouse.h"
#include "string.h"
#include "surface.h"
#include "visibility.h"

#include "font8x8_basic.h"
#include "ui_font.h"
#include "truetype.h"
#include "resources.h"

void* image_allocate(uint64_t size) { return kmalloc(size); }
void image_deallocate(void* pointer) { kfree(pointer); }

static resources::Catalog g_empty_catalog{};
static resources::Catalog* g_resources_catalog = &g_empty_catalog;
static volatile bool g_resources_requested = true;
static volatile bool g_resources_refresh = true;
static uint64_t g_resource_demand_generation = 0;
static unsigned g_resource_readers = 0;
// Publication waits for readers to finish. Readers never wait or touch disk.
struct ResourceRead {
    ResourceRead() { __atomic_add_fetch(&g_resource_readers, 1u, __ATOMIC_ACQUIRE); }
    ~ResourceRead() { __atomic_sub_fetch(&g_resource_readers, 1u, __ATOMIC_RELEASE); }
};
static inline uint64_t rdtsc_val();

static uint8_t* wm_resource_read(const char* path, uint64_t limit, uint64_t* size) {
    file f{}; *size=0;
    if (vfs_open(path,0,&f)!=0) return nullptr;
    uint64_t count=f.vn->size;
    uint8_t* data=f.vn->type==VnodeType::FILE && count && count<=limit ? (uint8_t*)kmalloc(count) : nullptr;
    uint64_t loaded=0,started=rdtsc_val();
    while (data && loaded<count && rdtsc_val()-started<_usb_tsc_hz()*15) {
        uint64_t chunk=count-loaded; if (chunk>65536) chunk=65536;
        if (vfs_read(&f,data+loaded,chunk)!=chunk) break;
        loaded+=chunk;
    }
    vfs_close(&f);
    if (!data || loaded!=count) { kfree(data); return nullptr; }
    *size=count; return data;
}
// Ranged read for .wiz metadata: fetches only `len` bytes at `offset`, so the
// desktop can read a name and icon from a package without loading all of it.
// len == 0 just reports the file size.
static bool wm_resource_range(const char* path, uint64_t offset, void* buffer, uint64_t len, uint64_t* total) {
    file f{};
    if (vfs_open(path,0,&f)!=0) return false;
    bool ok=f.vn->type==VnodeType::FILE;
    if (ok) *total=f.vn->size;
    if (ok && len) {
        ok=buffer && offset<=f.vn->size && len<=f.vn->size-offset;
        uint64_t done=0,started=rdtsc_val();
        if (ok) f.offset=offset;
        while (ok && done<len) {
            uint64_t chunk=len-done; if (chunk>65536) chunk=65536;
            if (vfs_read(&f,(uint8_t*)buffer+done,chunk)!=chunk || rdtsc_val()-started>_usb_tsc_hz()*15) ok=false;
            else done+=chunk;
        }
    }
    vfs_close(&f); return ok;
}
static int wm_resource_list(const char* path, uint64_t index, char* name, bool* directory) {
    uint64_t size=0; VnodeType type=VnodeType::FILE;
    int result=vfs_readdir(path,index,name,&size,&type);
    *directory=type==VnodeType::DIRECTORY; return result;
}

// ── RDTSC Helper ─────────────────────────────────────────────────────────────
static inline uint64_t rdtsc_val() {
    uint64_t val;
    __asm__ volatile("rdtsc; shl $32, %%rdx; or %%rdx, %%rax" : "=a"(val) :: "rdx");
    return val;
}

// Kernel globals defined in kernel.cpp / scheduler.h
extern uint64_t tsc_hz;
extern volatile uint64_t g_idle_tsc_accum;
extern uint64_t g_last_frame_ticks;

extern uint32_t g_term_max_cols, g_term_max_rows;

struct WinRect {
    int32_t x1, y1, x2, y2;
    bool visible;
};

static uint64_t g_time_bg_ticks = 0;
static uint64_t g_time_win_ticks = 0;
static uint64_t g_time_rain_ticks = 0;
static uint64_t g_time_lock_ticks = 0;
static uint64_t g_time_vram_ticks = 0;
// Capacity (chars) of the top-right HUD line. The compositor thread dirties a region this wide once a second,
// so EVERY part of the line is flipped to the screen, not just its right-hand end.
#define HUD_MSG_CAP 200
static uint64_t g_client_render_ticks = 0; // time the rainbow CLIENT spent drawing its own surface (outside the compositor)
// Snapshot of the PREVIOUS frame's complete timings, taken at the end of wm_compose_dirty().
// The HUD is built in the middle of a frame, so it must read these, not the live counters
// (which are reset at frame start and only filled in after the HUD has been drawn).
static uint64_t g_snap_bg = 0, g_snap_win = 0, g_snap_rain = 0, g_snap_lock = 0, g_snap_vram = 0;

// HUD helper: append a TSC-tick duration as milliseconds with one decimal ("1.9").
static char* hud_put_ms1(char* sp, uint64_t ticks) {
    uint64_t t10 = tsc_hz ? (ticks * 10000ULL / tsc_hz) : 0;   // tenths of a millisecond
    char tmp[24]; to_str(t10 / 10, tmp);
    for (char* q = tmp; *q; ) *sp++ = *q++;
    *sp++ = '.'; *sp++ = (char)('0' + (t10 % 10));
    return sp;
}

#define WIN_MOD_CTRL  (1u << 0)
#define WIN_MOD_SHIFT (1u << 1)
#define WIN_MOD_ALT   (1u << 2)
#define WIN_MOD_GUI   (1u << 3)

#define WIN_EVENT_NONE         0u
#define WIN_EVENT_MOUSE_DOWN   1u
#define WIN_EVENT_MOUSE_UP     2u
#define WIN_EVENT_MOUSE_MOVE   3u
#define WIN_EVENT_MOUSE_SCROLL 4u
#define WIN_EVENT_KEY_DOWN     5u
#define WIN_EVENT_KEY_UP       6u
#define WIN_EVENT_CLOSE        7u

struct WinEvent {
    uint32_t type;    // WIN_EVENT_*
    int32_t  x;       // client-relative x
    int32_t  y;       // client-relative y
    uint32_t key;     // ASCII or scancode / button (1=L, 2=R, 4=M)
    uint32_t mods;    // WIN_MOD_*
    int32_t  delta;   // scroll dy (+1 or -1)
};

#define WIN_EVENT_QUEUE_SIZE 64

struct Window {
    int id;
    char title[64];
    char executable[256];
    int32_t x, y, w, h;
    uint32_t* buffer;
    uint32_t buffer_pitch;
    uint64_t buffer_phys;   // physical base address of surface
    uint64_t buffer_pages;  // number of 4K pages
    uint64_t user_va;       // virtual address mapped into user space
    pid_t    owner_pid;     // task PID that owns this window
    bool visible;
    bool closable;
    bool chrome_drawn;
    bool is_system; // If true, handled specially (e.g. Terminal, Rain)
    bool close_pending; // separate from the bounded input queue: cannot be dropped
    int type;      // For system windows
    bool ghost_dragging;
    int32_t ghost_x, ghost_y;
    int32_t drag_off_x, drag_off_y;
    int32_t last_ghost_fx, last_ghost_fy, last_ghost_fw, last_ghost_fh;
    bool last_ghost_active;

    // Per-window event queue
    WinEvent event_queue[WIN_EVENT_QUEUE_SIZE];
    uint32_t event_head;
    uint32_t event_tail;
};

#define USER_WIN_BASE   0x0000600000000000ULL
#define USER_WIN_STRIDE 0x0000000010000000ULL // 256 MiB per window ID

#define MAX_WINDOWS 32
static Window* g_window_list[MAX_WINDOWS];
static int     g_window_order[MAX_WINDOWS];
static int     g_window_count = 0;
static Window* g_win_terminal = nullptr;
static Window* g_win_log = nullptr;
static Window* g_win_test = nullptr;
static Window* g_win_rain = nullptr;

static int      g_focused_win_id = -1;
static uint32_t g_kbd_modifiers = 0;

static inline bool wm_win_push_event(Window* win, const WinEvent& ev) {
    if (!win) return false;
    uint32_t next_head = (win->event_head + 1) % WIN_EVENT_QUEUE_SIZE;
    if (next_head == win->event_tail) {
        win->event_tail = (win->event_tail + 1) % WIN_EVENT_QUEUE_SIZE;
    }
    win->event_queue[win->event_head] = ev;
    win->event_head = next_head;
    return true;
}

static inline bool wm_win_pop_event(Window* win, WinEvent* out_ev) {
    if (!win || !out_ev) return false;
    if (win->close_pending) {
        win->close_pending = false;
        *out_ev = {};
        out_ev->type = WIN_EVENT_CLOSE;
        return true;
    }
    if (win->event_head == win->event_tail) return false;
    *out_ev = win->event_queue[win->event_tail];
    win->event_tail = (win->event_tail + 1) % WIN_EVENT_QUEUE_SIZE;
    return true;
}

static uint32_t g_title_h = 28;
static uint32_t g_border  = 2;
static uint32_t g_pad     = 2;
#define CURSOR_W    12
#define CURSOR_H    19

#define WIN_TYPE_TERM 0
#define WIN_TYPE_TEST 1
#define WIN_TYPE_RAIN 2
#define WIN_TYPE_LOG  3
#define WIN_TYPE_USER 10

// ─── Window surfaces: damage API ────────────────────────────────────────────
// A client draws into win->buffer (its SURFACE, origin = top-left of the client area), then calls
// wm_surface_damage*() with the rectangle that changed. The compositor never needs to know what
// the client is: damage is converted to a screen dirty-rect and every window is composed the same way.
static inline void wm_surface_damage(Window* win, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    if (!win || !win->visible || !fb) return;
    int32_t sx1, sy1, sx2, sy2;
    if (!surface_damage_to_screen(win->x, win->y, win->w, win->h, x1, y1, x2, y2,
                                  (int32_t)fb->width, (int32_t)fb->height, &sx1, &sy1, &sx2, &sy2)) return;
    uint32_t ux1 = (uint32_t)sx1, ux2 = (uint32_t)sx2 - 1;
    uint32_t uy1 = (uint32_t)sy1, uy2 = (uint32_t)sy2;
    if (uy2 > fb->height - 1) uy2 = fb->height - 1;
    wm_dirty_add(ux1, ux2, uy1, uy2);
    g_needs_refresh = true;
}
static inline void wm_damage_window_full(Window* win) {
    if (!win || !win->visible || !fb) return;
    int32_t x1 = win->x;
    int32_t y1 = win->y - (int32_t)g_title_h;
    int32_t x2 = win->x + win->w;
    int32_t y2 = win->y + win->h;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > (int32_t)fb->width) x2 = (int32_t)fb->width;
    if (y2 > (int32_t)fb->height) y2 = (int32_t)fb->height;
    if (x2 > x1 && y2 > y1) {
        wm_dirty_add((uint32_t)x1, (uint32_t)(x2 - 1), (uint32_t)y1, (uint32_t)(y2 - 1));
        g_needs_refresh = true;
    }
}

static inline void wm_surface_damage_all(Window* win) {
    if (!win) return;
    if (!win->chrome_drawn) {
        wm_damage_window_full(win);
        win->chrome_drawn = true;
    } else {
        wm_surface_damage(win, 0, 0, win->w, win->h);
    }
}

static uint32_t test_w, test_h;
static uint32_t rain_w, rain_h;
static uint32_t rain_scroll = 0;
static uint32_t g_rainbow_lut[360];
static uint32_t g_rainbow_wide[720]; // wide[j] = lut[j/2]: one entry per PIXEL, so a row is a plain cyclic window
static uint32_t wm_win_x, wm_win_y, wm_win_w, wm_win_h;
static uint32_t g_taskbar_h = 40; // Default, will be recalculated in wm_init
static uint32_t g_sb_w = 92;
static uint32_t g_wbtn_w = 124;

// ── Start Menu (taskbar popup) ────────────────────────────────
#define g_menu_app_count ((int)g_resources_catalog->count)
static bool  g_menu_open = false;

// Panel geometry (scaled like the rest of the UI). The app list starts below the header.
static inline void wm_menu_geom(int32_t* px, int32_t* py, int32_t* pw, int32_t* ph) {
    uint32_t sn = fb->height, sd = 1080;
    int32_t w     = (int32_t)((sn * 340) / sd); if (w < 250) w = 250;
    int32_t hdr_h = (int32_t)((sn * 36)  / sd); if (hdr_h < 24) hdr_h = 24;
    int32_t row_h = (int32_t)((sn * 38)  / sd); if (row_h < 26) row_h = 26;
    int32_t pad   = (int32_t)((sn * 10)  / sd); if (pad < 6) pad = 6;

    int rows = g_menu_app_count > 0 ? g_menu_app_count : 1;
    int32_t h = hdr_h + rows * row_h + pad * 2;
    int32_t max_h = (int32_t)((sn * 740) / sd);
    if (h > max_h) h = max_h;

    int32_t x = 6; // aligned with the Start button
    int32_t y = (int32_t)fb->height - (int32_t)g_taskbar_h - h - (int32_t)((sn * 4) / sd);
    if (y < 0) y = 0;
    *px = x; *py = y; *pw = w; *ph = h;
}

// Item index under the cursor, or -1. in_panel reports whether the point is inside the panel.
static inline int wm_menu_hit_test(int32_t mx, int32_t my, bool* in_panel = nullptr) {
    if (!g_menu_open || !fb) { if (in_panel) *in_panel = false; return -1; }
    int32_t px, py, pw, ph;
    wm_menu_geom(&px, &py, &pw, &ph);
    bool inside = (mx >= px && mx < px + pw && my >= py && my < py + ph);
    if (in_panel) *in_panel = inside;
    if (!inside) return -1;

    uint32_t sn = fb->height, sd = 1080;
    int32_t hdr_h = (int32_t)((sn * 36) / sd); if (hdr_h < 24) hdr_h = 24;
    int32_t row_h = (int32_t)((sn * 38) / sd); if (row_h < 26) row_h = 26;
    int32_t pad   = (int32_t)((sn * 10) / sd); if (pad < 6) pad = 6;

    int32_t list_top = py + hdr_h + pad;
    if (my < list_top || my >= list_top + g_menu_app_count * row_h) return -1;
    int idx = (my - list_top) / row_h;
    if (idx < 0 || idx >= g_menu_app_count) return -1;
    return idx;
}

// Rebuild the app list: /apps registrations, then /bin and /home/Applications (.wiz and .elf), excluding the shell.

static inline void wm_menu_refresh() {
    g_resources_refresh = true;
    ++g_resource_demand_generation;
    g_resources_requested = true;
}

// Mark the whole panel dirty so the compositor repaints it.
static inline void wm_menu_dirty_rect() {
    if (!fb) return;
    int32_t px, py, pw, ph;
    wm_menu_geom(&px, &py, &pw, &ph);
    uint32_t x1 = (uint32_t)(px > 0 ? px : 0), y1 = (uint32_t)(py > 0 ? py : 0);
    int32_t xe = px + pw - 1; // panel right edge, inclusive
    int32_t ye = py + ph - 1; // panel bottom edge, inclusive
    uint32_t x2 = (xe < (int32_t)fb->width)  ? (uint32_t)xe : fb->width - 1;
    uint32_t y2 = (ye < (int32_t)fb->height) ? (uint32_t)ye : fb->height - 1;
    wm_dirty_add(x1, x2, y1, y2);
    g_needs_refresh = true;
}

static inline void wm_menu_open() {
    g_menu_open = true;
    wm_menu_refresh();
    wm_menu_dirty_rect();
}

static inline void wm_menu_close() {
    if (!g_menu_open) return;
    g_menu_open = false;
    ++g_resource_demand_generation;
    g_resources_requested = true;
    wm_menu_dirty_rect(); // geometry unchanged: the list stays cached until next open
}

// Pointer moved over an open menu: repaint so hover highlights follow.
static inline void wm_menu_hover_dirty() {
    if (!g_menu_open) return;
    wm_menu_dirty_rect();
}

static uint32_t wm_icon_size(uint32_t value, uint32_t minimum, uint32_t maximum) {
    return value<minimum ? minimum : value>maximum ? maximum : value;
}
static resources::Store g_resource_store{{wm_resource_read,wm_resource_list,wm_resource_range}};
static void wm_resources_reload() {
    uint32_t row=wm_icon_size(fb->height*38/1080,26,256);
    resources::Demand demand{};
    uint64_t flags,generation;
    bool refresh;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    generation=g_resource_demand_generation;
    refresh=g_resources_refresh; g_resources_refresh=false;
    if (g_menu_open) {
        int32_t header=fb->height*36/1080; if (header<24) header=24;
        int32_t pad=fb->height*10/1080; if (pad<6) pad=6;
        int32_t room=(int32_t)(fb->height*740/1080)-header-pad*2;
        if (room>0) demand.menu_rows=(room+row-1)/row;
    }
    for (int i=0;i<g_window_count;++i) {
        const Window* win=g_window_list[i];
        if (!win || !win->visible) continue;
        if (win->is_system && win->type>=0 && win->type<4) demand.system[win->type]=true;
        else if (!win->is_system && demand.window_count<32) {
            resources::copy(demand.executables[demand.window_count++],256,win->executable);
            bool registered=false;
            for (unsigned k=0;k<g_resources_catalog->count;++k)
                if (resources::equal(win->executable,g_resources_catalog->apps[k].app.exec)) registered=true;
            for (unsigned k=0;k<g_resources_catalog->extra_count && k<resources::max_extra;++k)
                if (resources::equal(win->executable,g_resources_catalog->extra[k].app.exec)) registered=true;
            if (resources::equal(win->executable,"/share/system/console.elf"))demand.system[0]=true;
            else if (!registered) demand.system[4]=true;
        }
    }
    if (flags&(1u<<9)) __asm__ volatile("sti" ::: "memory");
    auto* next=resources::build(g_resource_store,wm_icon_size(row-10,14,128),
                                wm_icon_size(g_title_h-8,12,64),wm_icon_size(g_taskbar_h-16,16,64),
                                demand,refresh ? nullptr : g_resources_catalog);
    if (!next) { if (refresh) g_resources_refresh=true; return; }
    resources::Catalog* previous;
    // The compositor can be preempted halfway through a draw; never free its
    // snapshot or change the menu geometry until that read has finished.
    for (;;) {
        uint64_t flags;
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
        if (generation!=g_resource_demand_generation) {
            g_resources_requested=true;
            if (refresh) g_resources_refresh=true;
            if (flags&(1u<<9)) __asm__ volatile("sti" ::: "memory");
            resources::release(next); return;
        }
        if (!__atomic_load_n(&g_resource_readers,__ATOMIC_ACQUIRE)) {
            if (g_menu_open) wm_menu_dirty_rect();
            previous=g_resources_catalog; g_resources_catalog=next;
            wm_dirty_add(0,fb->width-1,0,fb->height-1); g_needs_refresh=true;
            if (flags&(1u<<9)) __asm__ volatile("sti" ::: "memory");
            break;
        }
        if (flags&(1u<<9)) __asm__ volatile("sti" ::: "memory");
        task_sleep_ms(1);
    }
    if (previous!=&g_empty_catalog) resources::release(previous);
}
static void wm_resource_worker() {
    for (;;) {
        if (g_resources_requested) {
            g_resources_requested=false;
            wm_resources_reload();
        }
        task_sleep_ms(50);
    }
}
static const resources::Icons& wm_window_icons(const Window* window) {
    if(resources::equal(window->executable,"/share/system/console.elf"))return g_resources_catalog->system[0];
    if (window->is_system && window->type>=0 && window->type<4)
        return g_resources_catalog->system[window->type];
    for (unsigned i=0;i<g_resources_catalog->count;++i)
        if (resources::equal(window->executable,g_resources_catalog->apps[i].app.exec))
            return g_resources_catalog->apps[i].icons;
    for (unsigned i=0;i<g_resources_catalog->extra_count && i<resources::max_extra;++i)
        if (resources::equal(window->executable,g_resources_catalog->extra[i].app.exec))
            return g_resources_catalog->extra[i].icons;
    return g_resources_catalog->system[4];
}
static void wm_draw_icon(const Image& image, int32_t x, int32_t y,
                          int32_t left, int32_t top, int32_t right, int32_t bottom) {
    image_draw(image,(uint32_t*)g_master_backbuffer,fb->width,fb->height,fb->pitch/4,
               x,y,left,top,right,bottom);
}

// Optional precomputed wallpaper backing store (same size as framebuffer).
// When present, the compositor samples wallpaper pixels from this buffer
// instead of a flat teal fill for the desktop background.
static uint32_t* g_wallpaper = nullptr;

static inline uint32_t wm_wallpaper_sample(uint32_t x, uint32_t y) {
    // Fallback color if framebuffer is not ready
    if (!fb) return 0xFF008080;

    // Radial "sunrise" gradient with a soft vignette and scanline bands.
    // All integer math: no libm or floating point required.
    uint32_t W = fb->width;
    uint32_t H = fb->height;
    uint32_t cx = W / 2;
    uint32_t cy = (H * 3) / 5; // Slightly below center for a horizon feel

    int32_t dx = (int32_t)x - (int32_t)cx;
    int32_t dy = (int32_t)y - (int32_t)cy;
    uint64_t dist2 = (uint64_t)dx * (uint64_t)dx + (uint64_t)dy * (uint64_t)dy;
    uint64_t max_dx = (uint64_t)cx * (uint64_t)cx;
    uint64_t max_dy = (uint64_t)cy * (uint64_t)cy;
    uint64_t max_dist2 = max_dx + max_dy;
    if (max_dist2 == 0) max_dist2 = 1;

    // 0 (center) → 255 (far corners)
    uint32_t t = (uint32_t)((dist2 * 255) / max_dist2);
    if (t > 255) t = 255;

    // Base gradient: deep blue at bottom → purple/azure toward top
    // Interpolate between two ARGB colors purely in integer space.
    uint32_t top_col    = 0xFF3A2F6C; // near zenith
    uint32_t bottom_col = 0xFF071726; // horizon band

    // Flip t so center is brightest
    uint32_t u = 255 - t;

    uint8_t r_top = (uint8_t)((top_col    >> 16) & 0xFF);
    uint8_t g_top = (uint8_t)((top_col    >>  8) & 0xFF);
    uint8_t b_top = (uint8_t)((top_col         ) & 0xFF);
    uint8_t r_bot = (uint8_t)((bottom_col >> 16) & 0xFF);
    uint8_t g_bot = (uint8_t)((bottom_col >>  8) & 0xFF);
    uint8_t b_bot = (uint8_t)((bottom_col      ) & 0xFF);

    uint8_t r = (uint8_t)(r_bot + ((int32_t)(r_top - r_bot) * (int32_t)u) / 255);
    uint8_t g = (uint8_t)(g_bot + ((int32_t)(g_top - g_bot) * (int32_t)u) / 255);
    uint8_t b = (uint8_t)(b_bot + ((int32_t)(b_top - b_bot) * (int32_t)u) / 255);

    // Vertical vignette: darken top/bottom edges slightly
    uint32_t vy = (y * 255) / (H ? H : 1);
    uint32_t v_factor = 220 + (vy > 128 ? (vy - 128) : (128 - vy)) / 6;
    if (v_factor > 255) v_factor = 255;
    r = (uint8_t)((uint32_t)r * v_factor / 255);
    g = (uint8_t)((uint32_t)g * v_factor / 255);
    b = (uint8_t)((uint32_t)b * v_factor / 255);

    // Subtle scanline banding: every 3rd line is slightly darker
    if ((y % 3u) == 0u) {
        r = (uint8_t)((uint32_t)r * 230 / 255);
        g = (uint8_t)((uint32_t)g * 230 / 255);
        b = (uint8_t)((uint32_t)b * 230 / 255);
    }

    return 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline void wm_init_wallpaper() {
    // If a raw wallpaper was already provided (e.g., from a Limine module),
    // keep it and skip the procedural generator.
    if (g_wallpaper) return;
    if (!fb) return;
    uint32_t W = fb->width;
    uint32_t H = fb->height;
    if (!W || !H) return;

    uint64_t total_px = (uint64_t)W * (uint64_t)H;
    g_wallpaper = (uint32_t*)kmalloc(total_px * sizeof(uint32_t));
    if (!g_wallpaper) return;

    for (uint32_t y = 0; y < H; y++) {
        uint32_t* row = g_wallpaper + (uint64_t)y * W;
        for (uint32_t x = 0; x < W; x++) {
            row[x] = wm_wallpaper_sample(x, y);
        }
    }
}

#include "wallpaper.h"

// Decode into a private buffer; publish only a complete validated wallpaper.
static bool wm_load_wallpaper_from_raw(const uint8_t* data, uint64_t size) {
    if (!fb || !fb->width || !fb->height || fb->width > 16384 || fb->height > 16384) return false;
    uint32_t* replacement = (uint32_t*)kmalloc((uint64_t)fb->width * fb->height * 4);
    if (!replacement) return false;
    Image decoded{},scaled{};
    bool standard=image_decode(data,size,&decoded);
    bool ok=standard && image_resize(decoded,fb->width,fb->height,&scaled);
    if (ok) {
        for (uint64_t i=0;i<(uint64_t)fb->width*fb->height;++i)
            replacement[i]=image_over(0xff171d30,scaled.pixels[i]);
    } else if (!standard) {
        bool encoded=(size>=8 && wallpaper_u32(data)==0x474e5089 && wallpaper_u32(data+4)==0x0a1a0a0d) ||
                     (size>=2 && data[0]==255 && data[1]==216);
        if (!encoded) ok=wallpaper_decode(data,size,fb->width,fb->height,replacement);
    }
    image_free(&scaled); image_free(&decoded);
    if (!ok) {
        kfree(replacement);
        return false;
    }
    // Single-core: exclude compositor preemption while replacing/freeing its source.
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    uint32_t* previous = g_wallpaper;
    g_wallpaper = replacement;
    kfree(previous);
    wm_dirty_add(0, fb->width - 1, 0, fb->height - 1);
    g_needs_refresh = true;
    if (flags & (1u << 9)) __asm__ volatile("sti" ::: "memory");
    return true;
}

static bool wm_load_wallpaper_file(const char* path) {
    file f{};
    if (vfs_open(path, 0, &f) != 0) {
        print((char*)"[wallpaper] Cannot open configured asset:");
        print((char*)path);
        return false;
    }
    uint64_t size = f.vn->size;
#if BOOT_DIAGNOSTICS
    char asset_size[32]; to_str(size, asset_size);
    print((char*)"[wallpaper] Asset bytes:"); print(asset_size);
#endif
    // Bound allocations even when disk metadata is corrupt.
    uint8_t* data = size && size <= 64 * 1024 * 1024 ? (uint8_t*)kmalloc(size) : nullptr;
    uint64_t loaded = 0;
    uint64_t started = rdtsc_val();
    uint64_t budget = _usb_tsc_hz() * 15;
    while (data && loaded < size) {
        if (rdtsc_val() - started >= budget) {
            print((char*)"[wallpaper] Disk read timed out; keeping fallback.");
            break;
        }
        uint64_t chunk = size - loaded;
        if (chunk > 65536) chunk = 65536;
        uint64_t count = vfs_read(&f, data + loaded, chunk);
        if (count != chunk) {
            print((char*)"[wallpaper] Short/failed disk read; loaded bytes:");
            char amount[32]; to_str(loaded + count, amount); print(amount);
            break;
        }
        loaded += count;
        boot_splash_tick();
    }
#if BOOT_DIAGNOSTICS
    char elapsed[32];
    to_str((rdtsc_val() - started) / (_usb_tsc_hz() / 1000), elapsed);
    print((char*)"[wallpaper] Disk-read milliseconds:"); print(elapsed);
    uint64_t decode_started = rdtsc_val();
#endif
    bool ok = data && loaded == size && wm_load_wallpaper_from_raw(data, size);
#if BOOT_DIAGNOSTICS
    to_str((rdtsc_val() - decode_started) / (_usb_tsc_hz() / 1000), elapsed);
    print((char*)"[wallpaper] Decode/scale milliseconds:"); print(elapsed);
#endif
    vfs_close(&f);
    if (data) kfree(data);
    return ok;
}

// Boot-only selection: publish the atlas before the shell/compositor starts.
static ttf::Atlas* g_desktop_font = nullptr;
static void wm_load_disk_font() {
    char path[256]; strcpy(path, "/share/fonts/selawk.ttf");
    file config{};
    if (vfs_open("/etc/font.cfg", 0, &config) == 0) {
        uint64_t n = config.vn->size;
        bool valid = n && n < sizeof(path) && vfs_read(&config, path, n) == n;
        vfs_close(&config);
        if (valid) {
            path[n] = 0;
            while (n && (path[n-1] == '\r' || path[n-1] == '\n')) path[--n] = 0;
            valid = n && path[0] == '/';
            for (uint64_t i=0; i<n; ++i) if ((uint8_t)path[i] < 32) valid = false;
        }
        if (!valid) { print((char*)"[font] Invalid /etc/font.cfg; using built-in Selawik."); return; }
    }
    file source{};
    if (vfs_open(path, 0, &source) != 0) {
        print((char*)"[font] Font unavailable; using built-in Selawik."); return;
    }
    uint64_t size = source.vn->size;
    uint8_t* data = size && size <= ttf::max_file ? (uint8_t*)kmalloc(size) : nullptr;
    uint64_t loaded = 0, started = rdtsc_val();
    while (data && loaded < size && rdtsc_val() - started < _usb_tsc_hz()*15) {
        uint64_t chunk = size-loaded; if (chunk>65536) chunk=65536;
        if (vfs_read(&source, data+loaded, chunk) != chunk) break;
        loaded += chunk; boot_splash_tick();
    }
    vfs_close(&source);
    ttf::Atlas* atlas = data && loaded == size ? (ttf::Atlas*)kmalloc(sizeof(ttf::Atlas)) : nullptr;
    ttf::Scratch* scratch = atlas ? (ttf::Scratch*)kmalloc(sizeof(ttf::Scratch)) : nullptr;
    bool ok = scratch && ttf::bake(data, size, *atlas, *scratch);
    kfree(scratch); kfree(data);
    if (!ok) { kfree(atlas); print((char*)"[font] Invalid/unreadable font; using built-in Selawik."); return; }
    g_desktop_font = atlas;
    ui_active_fonts = atlas->fonts; ui_active_font_count = 8;
    print((char*)"[font] Loaded desktop font:"); print(path);
}

// desktop.cfg is one absolute asset path, optionally followed by CR/LF.
static void wm_load_disk_wallpaper() {
    char path[256];
    strcpy(path, "/share/wall.raw");
    file config{};
    if (vfs_open("/etc/desktop.cfg", 0, &config) == 0) {
        char setting[256];
        uint64_t size = config.vn->size;
        if (size > 0 && size < sizeof(setting) && vfs_read(&config, setting, size) == size) {
            setting[size] = 0;
            while (size && (setting[size - 1] == '\n' || setting[size - 1] == '\r')) setting[--size] = 0;
            bool valid = size && setting[0] == '/';
            for (uint64_t i = 0; i < size; ++i) if ((uint8_t)setting[i] < 32) valid = false;
            if (valid) strcpy(path, setting);
        }
        vfs_close(&config);
    }
    print((char*)"[wallpaper] Loading disk asset...");
#if BOOT_DIAGNOSTICS
    uint64_t started = rdtsc_val();
#endif
    print((char*)(wm_load_wallpaper_file(path) ?
        "[wallpaper] Loaded disk asset into RAM." :
        "[wallpaper] Missing, unreadable or invalid asset; keeping fallback."));
#if BOOT_DIAGNOSTICS
    char elapsed[32];
    to_str((rdtsc_val() - started) / (_usb_tsc_hz() / 1000), elapsed);
    print((char*)"[wallpaper] Load attempt milliseconds:");
    print(elapsed);
#endif
}

// ── WIN_LOG: independent scrollable text window ──────────────────────────────
#define LOG_WIN_LINES 150
#define LOG_WIN_COLS  100
#define LOG_WIN_ROWS_VISIBLE 60
static uint32_t g_log_win_cols = 100;
static uint32_t g_log_rows_visible = 60;
static uint32_t g_log_stride = 800;
static uint32_t g_font_scale_100 = 100;
#define LOG_CHAR_W    8
#define LOG_CHAR_H    10
static int32_t  log_win_scroll = 0; // rows scrolled up (positive = older content)
static char     g_log_buf[LOG_WIN_LINES][LOG_WIN_COLS + 1];
static uint32_t g_log_line_count = 0;
static uint32_t* g_log_win_backbuffer = nullptr;
static int32_t  log_last_rendered_scroll = -1;

static inline void wm_get_win_rect(Window* win, WinRect* r) {
    r->visible = win->visible;
    r->x1 = win->x;
    r->y1 = win->y - (int32_t)g_title_h;
    r->x2 = win->x + (int32_t)win->w;
    r->y2 = win->y + (int32_t)win->h;
}

static inline void wm_get_win_rect_from_pos(Window* win, int32_t x, int32_t y, int32_t* fx, int32_t* fy, int32_t* fw, int32_t* fh) {
    *fx = x;
    *fy = y - (int32_t)g_title_h;
    *fw = win->w;
    *fh = win->h + (int32_t)g_title_h;
}

static void wm_log_render_line_to_buffer(int32_t line_idx, int32_t buffer_row) {
    if (!g_log_win_backbuffer) return;
    uint32_t stride = g_log_stride;
    const char* line = g_log_buf[line_idx];
    uint32_t py = buffer_row * LOG_CHAR_H;
    uint32_t row_color = (line_idx % 2 == 0) ? 0xFFCCDDFF : 0xFFAABBDD;

    // Clear background for the entire stride to avoid artifacts
    for (int r = 0; r < LOG_CHAR_H; r++) {
        memset_32(g_log_win_backbuffer + (py + r) * stride, 0xFF0a0a14, stride);
    }

    for (int c = 0; line[c] && c < (int32_t)g_log_win_cols; c++) {
        uint32_t px = c * LOG_CHAR_W;
        uint8_t* glyph = (uint8_t*)font8x8_basic[(uint8_t)line[c]];
        for (int r = 0; r < 8; r++) {
            uint32_t* buf_row = g_log_win_backbuffer + (py + r) * stride + px;
            for (int b = 0; b < 8; b++) {
                if (glyph[r] & (1 << b)) buf_row[b] = row_color;
            }
        }
    }
}

static void wm_log_render_to_buffer() {
    if (!g_log_win_backbuffer) return;
    if (log_win_scroll == log_last_rendered_scroll) return;

    uint32_t stride = g_log_stride;
    uint32_t bh = g_log_rows_visible * LOG_CHAR_H;

    // Delta-scroll optimization: if we moved by exactly 1 line, memmove and draw 1 line
    int32_t delta = log_win_scroll - log_last_rendered_scroll;
    if (log_last_rendered_scroll != -1 && delta == 1) {
        // Scrolled DOWN (content moves UP)
        memmove(g_log_win_backbuffer, g_log_win_backbuffer + stride * LOG_CHAR_H, stride * (g_log_rows_visible - 1) * LOG_CHAR_H * 4);
        wm_log_render_line_to_buffer(log_win_scroll + (int32_t)g_log_rows_visible - 1, (int32_t)g_log_rows_visible - 1);
    } else if (log_last_rendered_scroll != -1 && delta == -1) {
        // Scrolled UP (content moves DOWN)
        memmove(g_log_win_backbuffer + stride * LOG_CHAR_H, g_log_win_backbuffer, stride * (g_log_rows_visible - 1) * LOG_CHAR_H * 4);
        wm_log_render_line_to_buffer(log_win_scroll, 0);
    } else {
        // Full refresh
        memset_32(g_log_win_backbuffer, 0xFF0a0a14, stride * bh);
        for (int32_t r = 0; r < (int32_t)g_log_rows_visible; r++) {
            int32_t line_idx = log_win_scroll + r;
            if (line_idx >= 0 && line_idx < (int32_t)g_log_line_count)
                wm_log_render_line_to_buffer(line_idx, r);
        }
    }
    log_last_rendered_scroll = log_win_scroll;
}

static void wm_log_init() {
    for (int i = 0; i < LOG_WIN_LINES; i++) {
        // Fill with sample text so we have content to scroll
        const char* prefix = "Log line ";
        int j = 0;
        while (prefix[j] && j < LOG_WIN_COLS) { g_log_buf[i][j] = prefix[j]; j++; }
        // Append line number
        uint32_t n = i + 1;
        char tmp[8]; int tl = 0;
        if (n == 0) { tmp[tl++] = '0'; }
        else { uint32_t x = n; while (x) { tmp[tl++] = '0' + x % 10; x /= 10; } }
        for (int k = tl - 1; k >= 0 && j < LOG_WIN_COLS; k--) g_log_buf[i][j++] = tmp[k];
        // Pad rest with info chars to make lines visually substantial
        const char* info = " | compositor test | abcdefghijklmnop";
        int il = 0;
        while (info[il] && j < LOG_WIN_COLS) { g_log_buf[i][j] = info[il]; j++; il++; }
        g_log_buf[i][j] = '\0';
    }
    g_log_line_count = LOG_WIN_LINES;
    
    g_log_stride = g_log_win_cols * LOG_CHAR_W;
    if (g_win_log && g_win_log->buffer) {
        g_log_win_backbuffer = g_win_log->buffer;
    } else {
        uint32_t total_px = g_log_stride * (g_log_rows_visible * LOG_CHAR_H);
        g_log_win_backbuffer = (uint32_t*)kmalloc(total_px * 4);
    }
    wm_log_render_to_buffer();
    if (g_win_log) wm_surface_damage_all(g_win_log);
}

void wm_log_append(const char* s) {
    if (!s) return;
    
    // 1. Shift existing lines up if buffer is full
    if (g_log_line_count >= LOG_WIN_LINES) {
        memmove(g_log_buf[0], g_log_buf[1], (LOG_WIN_LINES - 1) * (LOG_WIN_COLS + 1));
        g_log_line_count = LOG_WIN_LINES - 1;
    }
    
    // 2. Copy the new line
    char* dst = g_log_buf[g_log_line_count];
    uint32_t i = 0;
    while (s[i] && i < LOG_WIN_COLS) {
        if (s[i] == '\n' || s[i] == '\r') break; // single line only
        dst[i] = s[i];
        i++;
    }
    dst[i] = '\0';
    g_log_line_count++;
    
    // 3. Auto-scroll to bottom
    int32_t max_scroll = (int32_t)g_log_line_count - (int32_t)g_log_rows_visible;
    if (max_scroll < 0) max_scroll = 0;
    log_win_scroll = max_scroll;
    
    // 4. Force a render to the log window's backbuffer
    log_last_rendered_scroll = -1; // force full refresh
    wm_log_render_to_buffer();

    // 5. Mark log window surface dirty
    if (g_win_log) {
        wm_surface_damage_all(g_win_log);
    }
}

static void wm_log_scroll(int delta) {
    log_win_scroll += delta;
    if (log_win_scroll < 0) log_win_scroll = 0;
    int32_t max_scroll = (int32_t)g_log_line_count - (int32_t)g_log_rows_visible;
    if (max_scroll < 0) max_scroll = 0;
    if (log_win_scroll > max_scroll) log_win_scroll = max_scroll;
    
    // Prerender to backbuffer
    wm_log_render_to_buffer();

    // Mark log window surface dirty
    if (g_win_log) {
        wm_surface_damage_all(g_win_log);
    }
}


static inline void wm_draw_mono_char(uint32_t x, uint32_t y, char c, uint32_t color, uint32_t scale_100 = 100) {
    if (!g_master_backbuffer || (uint32_t)c > 127) return;
    uint8_t* glyph = (uint8_t*)font8x8_basic[(uint8_t)c];
    
    if (scale_100 == 150) {
        // 1.5x custom pattern (8px -> 12px)
        for (int r = 0; r < 8; r++) {
            int dr = (r / 2) * 3 + (r % 2 == 1 ? 2 : 0);
            int rh = (r % 2 == 0) ? 2 : 1;
            for (int rsy = 0; rsy < rh; rsy++) {
                uint32_t dy = y + (uint32_t)(dr + rsy);
                if (dy >= fb->height) continue;
                uint32_t* row = (uint32_t*)(g_master_backbuffer + (uint64_t)dy * fb->pitch);
                for (int b = 0; b < 8; b++) {
                    if (glyph[r] & (1 << b)) {
                        int db = (b / 2) * 3 + (b % 2 == 1 ? 2 : 0);
                        int bw = (b % 2 == 0) ? 2 : 1;
                        for (int bsx = 0; bsx < bw; bsx++) {
                            uint32_t dx = x + (uint32_t)(db + bsx);
                            if (dx < fb->width) row[dx] = color;
                        }
                    }
                }
            }
        }
    } else {
        // Generic integer scaling
        uint32_t s = scale_100 / 100;
        if (s < 1) s = 1;
        for (int r = 0; r < 8; r++) {
            for (uint32_t sy = 0; sy < s; sy++) {
                uint32_t dy = y + r * s + sy;
                if (dy >= fb->height) continue;
                uint32_t* row = (uint32_t*)(g_master_backbuffer + (uint64_t)dy * fb->pitch);
                for (int b = 0; b < 8; b++) {
                    if (glyph[r] & (1 << b)) {
                        for (uint32_t sx = 0; sx < s; sx++) {
                            uint32_t dx = x + b * s + sx;
                            if (dx < fb->width) row[dx] = color;
                        }
                    }
                }
            }
        }
    }
}

static inline void wm_draw_mono_string(uint32_t x, uint32_t y, const char* s, uint32_t color, uint32_t scale_100 = 100) {
    uint32_t char_w = (8 * scale_100) / 100;
    for (int i = 0; s[i]; i++) {
        wm_draw_mono_char(x + i * char_w, y, s[i], color, scale_100);
    }
}

// Selawik UI typography. Shell, boot log and diagnostics keep the bitmap font.
static int32_t g_ui_clip_left = 0, g_ui_clip_top = 0, g_ui_clip_right = 0, g_ui_clip_bottom = 0;
static const UiFont* wm_ui_font(uint32_t scale_100 = 100) {
    uint32_t base = !fb || fb->height < 600 ? 12 : fb->height < 900 ? 14 : 16;
    return ui_font_for_pixels((base * scale_100 + 50) / 100);
}
static uint32_t wm_text_height(uint32_t scale_100 = 100) { return wm_ui_font(scale_100)->height; }
static int32_t wm_text_width(const char* text, uint32_t scale_100 = 100) {
    return ui_text_width(wm_ui_font(scale_100), text);
}
static void wm_draw_string(int32_t x, int32_t y, const char* text, uint32_t color,
                           uint32_t scale_100 = 100, int32_t max_width = -1) {
    if (!fb || !g_master_backbuffer || !text || max_width == 0) return;
    int32_t right = g_ui_clip_right;
    if (max_width > 0 && x + max_width < right) right = x + max_width;
    char shortened[128];
    if (max_width > 0 && wm_text_width(text, scale_100) > max_width) {
        int32_t width = 0, dots = wm_text_width("...", scale_100);
        unsigned n = 0;
        while (text[n] && n < sizeof(shortened) - 4) {
            unsigned advance = ui_font_glyph(wm_ui_font(scale_100), (uint8_t)text[n])->advance;
            if (width + (int32_t)advance + dots > max_width) break;
            width += advance; shortened[n] = text[n]; ++n;
        }
        if (dots <= max_width) { shortened[n++] = '.'; shortened[n++] = '.'; shortened[n++] = '.'; }
        shortened[n] = 0;
        text = shortened;
    }
    ui_text_draw((uint32_t*)g_master_backbuffer, fb->width, fb->height, fb->pitch / 4,
                 wm_ui_font(scale_100), x, y, text, color, g_ui_clip_left, g_ui_clip_top, right, g_ui_clip_bottom);
}
static void wm_draw_char(int32_t x, int32_t y, char c, uint32_t color, uint32_t scale_100 = 100) {
    char text[2]; text[0] = c; text[1] = 0;
    wm_draw_string(x, y, text, color, scale_100);
}
static uint32_t wm_clock_scale() { return g_font_scale_100 * 85 / 100; }

// ─── Taskbar clock (tray) ────────────────────────────────────────────────────
// The time comes from rtc.h (g_clock, refreshed by the compositor thread). Drawing only reads that cache.
// Tray width follows the font scale; the widest line is "10:59:59 PM" (11 chars) or the date "2026-11-06" (10).
static inline int32_t wm_tray_w() {
    uint32_t scale = wm_clock_scale();
    int32_t time_width = wm_text_width(CLOCK_SHOW_SECONDS ? "88:88:88 PM" : "88:88 PM", scale);
    int32_t date_width = wm_text_width("8888-88-88", scale);
    return (time_width > date_width ? time_width : date_width) + 20;
}

// Mark the tray (plus the 2px separator left of it) dirty so the compositor repaints the clock.
static inline void wm_clock_damage() {
    if (!fb) return;
    int32_t x1 = (int32_t)fb->width - wm_tray_w() - 6 - 8;
    if (x1 < 0) x1 = 0;
    int32_t y1 = (int32_t)fb->height - (int32_t)g_taskbar_h;
    if (y1 < 0) y1 = 0;
    wm_dirty_add((uint32_t)x1, fb->width - 1, (uint32_t)y1, fb->height - 1);
    g_needs_refresh = true;
}

static inline void wm_draw_window_chrome(int32_t x, int32_t y, int32_t w, int32_t h, const char* title, bool closable, int32_t min_y, int32_t max_y, bool focused = false, const Image* icon = nullptr) {
    if (!g_master_backbuffer) return;
    // Title bar background: active blue accent if focused, muted dark grey if inactive
    uint32_t bar_clr  = focused ? 0xFF2A4268 : 0xFF22242B;
    uint32_t bar_line = focused ? 0xFF4A6898 : 0xFF2D3039;
    for (int32_t ty = y; ty < y + (int32_t)g_title_h; ty++) {
        if (ty < 0 || (uint32_t)ty >= fb->height) continue;
        if (ty < min_y || ty > max_y) continue; // Respect dirty rect
        uint32_t* row = (uint32_t*)(g_master_backbuffer + (uint64_t)ty * fb->pitch);
        int32_t start_x = x; if (start_x < 0) start_x = 0;
        int32_t end_x = x + w; if (end_x > (int32_t)fb->width) end_x = fb->width;
        if (end_x > start_x) {
            uint32_t c = (ty == y) ? bar_line : bar_clr;
            memset_32(row + start_x, c, end_x - start_x);
        }
    }
    // Title text (Vertically centered)
    uint32_t char_h = wm_text_height(g_font_scale_100);
    int32_t text_y = y + (int32_t)(g_title_h - char_h) / 2;
    uint32_t text_clr = focused ? 0xFFFFFFFF : 0xFF9094A0;
    int32_t text_x=x+10;
    if (icon && icon->pixels) {
        wm_draw_icon(*icon,x+6,y+((int32_t)g_title_h-(int32_t)icon->height)/2,
                     x,min_y,x+w-(int32_t)g_title_h,max_y+1);
        text_x=x+12+icon->width;
    }
    int32_t title_space=w-(int32_t)g_title_h-(text_x-x)-14;
    if (title_space>0 && text_y + (int32_t)char_h >= min_y && text_y <= max_y)
        wm_draw_string(text_x, text_y, title, text_clr, g_font_scale_100, title_space);
    
    // Close button (Scaled with title bar)
    int32_t btn_pad = (int32_t)(4 * g_title_h) / 42;
    if (btn_pad < 2) btn_pad = 2;
    int32_t btn_h = (int32_t)g_title_h - btn_pad * 2;
    int32_t btn_w = btn_h;
    int32_t btn_x = x + w - btn_w - btn_pad;
    int32_t btn_y = y + btn_pad;

    uint32_t btn_clr = closable ? 0xFFFF0000 : 0xFF555555;
    for (int32_t by = btn_y; by < btn_y + btn_h; by++) {
        if (by < 0 || (uint32_t)by >= fb->height) continue;
        if (by < min_y || by > max_y) continue;
        uint32_t* row = (uint32_t*)(g_master_backbuffer + (uint64_t)by * fb->pitch);
        int32_t s_x = btn_x; if (s_x < 0) s_x = 0;
        int32_t e_x = btn_x + btn_w; if (e_x > (int32_t)fb->width) e_x = fb->width;
        if (e_x > s_x) memset_32(row + s_x, btn_clr, e_x - s_x);
    }
    // Centered 'X' in button
    int32_t x_off = (btn_w - wm_text_width("X", g_font_scale_100)) / 2;
    int32_t y_off = (btn_h - (int32_t)char_h) / 2;
    if (btn_y + y_off <= max_y && btn_y + y_off + (int32_t)char_h >= min_y)
        wm_draw_char(btn_x + x_off, btn_y + y_off, 'X', 0xFFFFFFFF, g_font_scale_100);
}

// =============================================================================
//  OVERLAY LAYER — cursor + ghost drag outlines
//
//  These are painted DIRECTLY onto VRAM. master_backbuffer is ALWAYS
//  cursor-free. This decouples cursor speed from compositor cost entirely,
//  the same way a hardware sprite overlay works.
// =============================================================================

// Currently live on VRAM beyond master_backbuffer:
static int32_t ov_cx = -999, ov_cy = -999;

// ─── Low-level VRAM helpers ──────────────────────────────────────────────────

// Restore a rectangle in VRAM from the cursor-free master_backbuffer.
// This is the only "erase" we ever need for overlays.
static inline void vram_restore_rect(int32_t x1, int32_t y1,
                                      int32_t x2, int32_t y2) {
    if (!fb || !g_master_backbuffer) return;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if ((uint32_t)x2 >= fb->width)  x2 = (int32_t)fb->width  - 1;
    if ((uint32_t)y2 >= fb->height) y2 = (int32_t)fb->height - 1;
    if (x1 > x2 || y1 > y2) return;
    uint32_t bw = (uint32_t)(x2 - x1 + 1) * 4;
    for (int32_t y = y1; y <= y2; y++) {
        uint64_t off = (uint64_t)y * fb->pitch + (uint64_t)x1 * 4;
        memcpy_vram_sse_headless((uint8_t*)fb->address + off,
                                  g_master_backbuffer   + off, bw);
    }
}

// Erase only the 4 border strips of a ghost outline (not the interior).
// Much cheaper than restoring a full bounding box for large windows.
// Single top-to-bottom pass that erases the old outline and draws the new one
// simultaneously. Every scanline is fully settled (old pixels restored, new
// pixels written) before we advance, so the display can never catch a row
// where the old left/right border has vanished but the new one hasn't landed.
//
// has_old=false → draw only (no erase).  has_new=false → erase only.
// w/h are frame dimensions as used by vram_draw_outline (x2 = x + w - 1).
static inline void vram_transition_outline(
    int32_t ox, int32_t oy, int32_t ow, int32_t oh, bool has_old,
    int32_t nx, int32_t ny, int32_t nw, int32_t nh, bool has_new)
{
    if (!fb || !g_master_backbuffer) return;
    if (!has_old && !has_new) return;

    const uint32_t W = fb->width, H = fb->height;
    const uint32_t P = fb->pitch / 4;
    const uint32_t COL_OUTER = 0xFFFFFFFF, COL_INNER = 0xFF888888;

    // Right-edge pixel (vram_draw_outline uses x2 = x + w - 1)
    int32_t ox2 = ox + ow - 1, oy2 = oy + oh - 1;
    int32_t nx2 = nx + nw - 1, ny2 = ny + nh - 1;

    // Y range covering everything we need to touch
    int32_t y_lo = 0x7fffffff, y_hi = -0x7fffffff;
    if (has_old) { y_lo = oy - 2; y_hi = oy2 + 2; }
    if (has_new) {
        if (ny - 2 < y_lo) y_lo = ny - 2;
        if (ny2 + 2 > y_hi) y_hi = ny2 + 2;
    }
    if (y_lo < 0) y_lo = 0;
    if (y_hi >= (int32_t)H) y_hi = (int32_t)H - 1;
    if (y_lo > y_hi) return;

    for (int32_t y = y_lo; y <= y_hi; y++) {
        uint32_t* vrow = (uint32_t*)((uint8_t*)fb->address      + (uint64_t)y * fb->pitch);
        
        // ── Step 1: restore old outline's pixels on this scanline ────────────
        if (has_old) {
            bool on_top = (y >= oy - 2 && y <= oy + 3);
            bool on_bot = (y >= oy2 - 3 && y <= oy2 + 2);
            bool on_ls  = (y >= oy + 4 && y <= oy2 - 4);
            
            auto restore_strip = [&](int32_t ix1, int32_t ix2) {
                if (ix1 < 0) ix1 = 0; 
                if (ix2 >= (int32_t)W) ix2 = (int32_t)W - 1;
                if (ix1 <= ix2) {
                    uint64_t off = (uint64_t)y * fb->pitch + (uint64_t)ix1 * 4;
                    uint32_t len = (uint32_t)(ix2 - ix1 + 1) * 4;
                    // Use scalar memcpy for these tiny 24-byte strips; SSE overhead 
                    // and alignment quirks on small lengths can cause artifacts.
                    memcpy((uint8_t*)fb->address + off, g_master_backbuffer + off, len);
                }
            };

            if (on_top || on_bot) {
                restore_strip(ox - 2, ox2 + 2);
            } else if (on_ls) {
                // ONLY restore the left side and right side borders!
                restore_strip(ox - 2, ox + 3);
                restore_strip(ox2 - 3, ox2 + 2);
            }
        }

        // ── Step 2: draw new outline's pixels ────────────────────────────────
        if (has_new) {
            uint32_t clr = 0xFFFFFFFF;
            if (y >= ny && y <= ny + 1) { // Top hlines
                int32_t ix1 = nx, ix2 = nx + nw - 1;
                if (ix1 < 0) ix1 = 0; if (ix2 >= (int32_t)W) ix2 = (int32_t)W - 1;
                if (ix1 <= ix2) for (int32_t sx = ix1; sx <= ix2; sx++) vrow[sx] = clr;
            } else if (y >= ny2 - 2 && y <= ny2 - 1) { // Bottom hlines
                int32_t ix1 = nx, ix2 = nx + nw - 1;
                if (ix1 < 0) ix1 = 0; if (ix2 >= (int32_t)W) ix2 = (int32_t)W - 1;
                if (ix1 <= ix2) for (int32_t sx = ix1; sx <= ix2; sx++) vrow[sx] = clr;
            } else if (y >= ny && y < ny2) { // Vertical edges
                if (nx >= 0 && nx < (int32_t)W) { vrow[nx] = clr; if (nx + 1 < (int32_t)W) vrow[nx+1] = clr; }
                int32_t nx_r = nx + nw - 1;
                if (nx_r >= 0 && nx_r < (int32_t)W) { vrow[nx_r] = clr; if (nx_r - 1 >= 0) vrow[nx_r-1] = clr; }
            }
        }
    }
}

// Draw a 2px ghost outline rectangle directly to VRAM (not master_backbuffer).
static inline void vram_draw_outline(int32_t x, int32_t y,
                                      int32_t w, int32_t h) {
    if (!fb) return;
    uint32_t* vram = (uint32_t*)fb->address;
    uint32_t  P    = fb->pitch / 4;
    int32_t   x2   = x + w - 1, y2 = y + h - 1;

    const uint32_t outer = 0xFFFFFFFF, inner = 0xFF888888;

    auto safe_hline = [&](int32_t lx1, int32_t lx2, int32_t ly, uint32_t c) {
        if (ly < 0 || (uint32_t)ly >= fb->height) return;
        if (lx1 < 0) lx1 = 0;
        if ((uint32_t)lx2 >= fb->width) lx2 = (int32_t)fb->width - 1;
        uint32_t* row = vram + (uint64_t)ly * P;
        for (int32_t lx = lx1; lx <= lx2; lx++) row[lx] = c;
    };
    auto safe_vline = [&](int32_t lx, int32_t ly1, int32_t ly2, uint32_t c) {
        if (lx < 0 || (uint32_t)lx >= fb->width) return;
        for (int32_t ly = ly1; ly <= ly2; ly++) {
            if (ly < 0 || (uint32_t)ly >= fb->height) continue;
            vram[ly * P + lx] = c;
        }
    };

    safe_hline(x,   x2,   y,    outer); safe_hline(x+1, x2-1, y+1,  inner);
    safe_hline(x,   x2,   y2,   outer); safe_hline(x+1, x2-1, y2-1, inner);
    safe_vline(x,   y+2,  y2-2, outer); safe_vline(x+1, y+2,  y2-2, inner);
    safe_vline(x2,  y+2,  y2-2, outer); safe_vline(x2-1,y+2,  y2-2, inner);
}

// ─── Rainbow Window Generator ───────────────────────────────────────────────

static inline uint32_t wm_rainbow_color(uint32_t x) {
    uint32_t h = (x / 2) % 360;
    uint32_t s = 255, v = 255;
    uint32_t r, g, b;
    uint32_t i = h / 60;
    uint32_t f = h % 60;
    uint32_t p = (v * (255 - s)) >> 8;
    uint32_t q = (v * (255 * 60 - s * f)) / (255 * 60);
    uint32_t t = (v * (255 * 60 - s * (60 - f))) / (255 * 60);
    switch (i) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

// Fast path (exactly equivalent to wm_draw_rainbow_content):
// pixel x of a rainbow row is g_rainbow_wide[(base_scroll + offset_x + x) % 720]. The colour
// depends only on x and the scroll, never on y, so the compositor builds ONE row and copies it
// to every other row instead of re-deriving ~600 identical rows per frame.
static inline void wm_fill_rainbow_row(uint32_t* dst, int32_t w, uint32_t base_scroll, uint32_t offset_x) {
    uint32_t pos = (uint32_t)(((uint64_t)base_scroll + offset_x) % 720u);
    while (w > 0) {
        uint32_t n = 720u - pos;
        if (n > (uint32_t)w) n = (uint32_t)w;
        memcpy(dst, &g_rainbow_wide[pos], (uint64_t)n * 4);
        dst += n; w -= (int32_t)n; pos = 0;
    }
}

static inline void wm_draw_rainbow_content(uint32_t* row, int32_t rw, uint32_t base_scroll, uint32_t offset_x) {
    uint32_t total = base_scroll + offset_x;
    uint32_t idx = (total / 2) % 360;
    uint32_t parity = total & 1;

    for (int32_t x = 0; x < rw; x++) {
        row[x] = g_rainbow_lut[idx];
        if (parity) {
            idx++;
            if (idx >= 360) idx = 0;
        }
        parity ^= 1;
    }
}

// Draw the cursor directly to VRAM. Does NOT modify master_backbuffer.
static inline void vram_draw_cursor(int32_t mx, int32_t my, uint8_t mb) {
    if (!fb) return;
    for (int cr = 0; cr < CURSOR_H; cr++) {
        int32_t sy = my + cr;
        if (sy < 0 || (uint32_t)sy >= fb->height) continue;
        uint32_t* row = (uint32_t*)((uint8_t*)fb->address + (uint64_t)sy * fb->pitch);
        for (int x = 0; x < CURSOR_W; x++) {
            int32_t sx = mx + x;
            if (sx < 0 || (uint32_t)sx >= fb->width) continue;
            uint8_t p = g_cursor_bitmap[cr][x];
            if (!p) continue;
            row[sx] = (p == 2)
                ? ((mb & 1) ? 0xFFFF0000 : (mb & 2) ? 0xFF00FFFF : 0xFF000000)
                : 0xFFFFFFFF;
        }
    }
}

// ─── Ghost frame geometry ────────────────────────────────────────────────────

static inline int wm_find_top_window(int32_t mx, int32_t my) {
    for (int s = g_window_count - 1; s >= 0; s--) {
        int idx = g_window_order[s];
        Window* win = g_window_list[idx];
        if (!win || !win->visible) continue;
        WinRect r; wm_get_win_rect(win, &r);
        if (mx >= r.x1 && mx < r.x2 && my >= r.y1 && my < r.y2) return idx;
    }
    return -1;
}

static inline void wm_raise_window(int idx) {
    if (idx < 0 || idx >= g_window_count) return;
    Window* win = g_window_list[idx];
    if (win) g_focused_win_id = win->id;
    int old_pos = -1;
    for (int i = 0; i < g_window_count; i++) if (g_window_order[i] == idx) old_pos = i;
    if (old_pos != -1 && old_pos < g_window_count - 1) {
        for (int i = old_pos; i < g_window_count - 1; i++) g_window_order[i] = g_window_order[i+1];
        g_window_order[g_window_count - 1] = idx;
    }
    if (fb) {
        g_dirty_min_x = 0; g_dirty_max_x = fb->width - 1;
        g_dirty_min_y = 0; g_dirty_max_y = fb->height - 1;
    }
    g_needs_refresh = true;
}

static inline bool wm_is_in_title(int idx, int32_t mx, int32_t my) {
    if (idx < 0 || idx >= g_window_count) return false;
    Window* win = g_window_list[idx];
    WinRect r; wm_get_win_rect(win, &r);
    return (mx >= r.x1 && mx < r.x2 && my >= r.y1 && my < r.y1 + (int32_t)g_title_h);
}

static inline bool wm_is_in_close(int idx, int32_t mx, int32_t my) {
    if (idx < 0 || idx >= g_window_count) return false;
    Window* win = g_window_list[idx];
    WinRect r; wm_get_win_rect(win, &r);
    
    int32_t w = r.x2 - r.x1;
    int32_t btn_pad = (int32_t)(4 * g_title_h) / 42;
    if (btn_pad < 2) btn_pad = 2;
    int32_t btn_h = (int32_t)g_title_h - btn_pad * 2;
    int32_t btn_w = btn_h;
    int32_t btn_x = r.x1 + w - btn_w - btn_pad;
    int32_t btn_y = r.y1 + btn_pad;
    
    return (mx >= btn_x && mx < btn_x + btn_w && my >= btn_y && my < btn_y + btn_h);
}

static inline void wm_close_window(int idx) {
    if (idx < 0 || idx >= g_window_count) return;
    Window* win = g_window_list[idx];
    if (win->is_system && win->type == WIN_TYPE_TERM) return; // Terminal is immortal
    if (!win->is_system) {
        // Applications decide whether to exit, close one of several windows,
        // or ask the user to save. Keep the surface alive until they respond.
        win->close_pending = true;
        return;
    }
    win->visible = false;
    ++g_resource_demand_generation; g_resources_requested=true;
    if (g_focused_win_id == win->id) {
        g_focused_win_id = -1;
        for (int s = g_window_count - 1; s >= 0; s--) {
            int o_idx = g_window_order[s];
            Window* w = g_window_list[o_idx];
            if (w && w->visible) {
                g_focused_win_id = w->id;
                break;
            }
        }
    }
    if (fb) {
        g_dirty_min_x = 0; g_dirty_max_x = fb->width - 1;
        g_dirty_min_y = 0; g_dirty_max_y = fb->height - 1;
    }
    g_needs_refresh = true;
}

static int g_next_win_id = 1;

static inline Window* wm_create_window(const char* title, int32_t x, int32_t y, int32_t w, int32_t h, bool closable, bool is_system = false, int type = WIN_TYPE_USER, pid_t owner = 0) {
    if (g_window_count >= MAX_WINDOWS) return nullptr;
    if (w < 32 || h < 32 || w > 3840 || h > 2160) return nullptr;
    
    Window* win = (Window*)kmalloc(sizeof(Window));
    if (!win) return nullptr;
    memset(win, 0, sizeof(Window));
    
    win->id = g_next_win_id++;
    uint32_t i = 0;
    while (title[i] && i < 63) { win->title[i] = title[i]; i++; }
    win->title[i] = '\0';
    
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->closable = closable;
    win->visible = true;
    win->is_system = is_system;
    win->type = type;
    win->owner_pid = owner;
    if (!is_system && g_current_task && g_current_task->pid==owner)
        resources::copy(win->executable,sizeof(win->executable),g_current_task->executable);
    
    uint64_t buf_bytes = (uint64_t)w * h * 4;
    uint64_t pages = (buf_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    win->buffer_pages = pages;
    
    if (!is_system) {
        // User windows: allocate contiguous physical pages so they can be mapped to user space
        uint64_t phys = alloc_phys_pages(pages);
        if (!phys) {
            kfree(win);
            return nullptr;
        }
        win->buffer_phys = phys;
        win->buffer = (uint32_t*)(HHDM + phys);
    } else {
        win->buffer_phys = 0;
        win->buffer = (uint32_t*)kmalloc(buf_bytes);
        if (win->buffer) memset(win->buffer, 0, buf_bytes);
    }
    win->buffer_pitch = w * 4;
    
    int idx = g_window_count;
    g_window_list[idx] = win;
    g_window_order[idx] = idx;
    g_window_count++;
    ++g_resource_demand_generation; g_resources_requested=true;
    
    wm_damage_window_full(win);
    return win;
}

static inline Window* wm_find_window(int id) {
    for (int i = 0; i < g_window_count; i++)
        if (g_window_list[i] && g_window_list[i]->id == id) return g_window_list[i];
    return nullptr;
}

extern void tty_input(char c);
extern void term_write_string(char* s);

static inline void wm_dispatch_key(char c, uint32_t scancode, bool is_down, uint32_t mods) {
    g_kbd_modifiers = mods;
    if (!is_down) return;
    Window* focused = wm_find_window(g_focused_win_id);
    if (focused && focused->is_system && focused->type==WIN_TYPE_TERM &&
        (mods & WIN_MOD_CTRL) && (c == 'c' || c == 'C' || c == 3)) {
        task_request_interrupt(g_terminal_foreground_pid);
        return; // consume the chord, including repeats after the process exits
    }

    // Focus-based routing
    if (!focused) {
        // Nothing selected / Desktop has focus: don't route to any window
        return;
    }

    if (focused->is_system && focused->type == WIN_TYPE_TERM) {
        if (c) tty_input(c);
        return;
    }

    if (focused->is_system && focused->type == WIN_TYPE_LOG) {
        if (c == 'j') wm_log_scroll(1);
        else if (c == 'k') wm_log_scroll(-1);
        return;
    }

    // Focused is a GUI / User window!
    WinEvent ev;
    ev.type = WIN_EVENT_KEY_DOWN;
    ev.x = 0;
    ev.y = 0;
    ev.key = c ? (uint32_t)(uint8_t)c : scancode;
    ev.mods = mods;
    ev.delta = 0;
    wm_win_push_event(focused, ev);
}

// Current client surfaces are opaque. Only windows above this client can hide
// its animation; ghost outlines are overlays and must not count as occluders.
static inline void wm_visible_client(Window* win, VisibleRegion* region) {
    region->count = 0;
    if (!win || !win->visible || !win->buffer || !fb) return;
    int32_t width = (int32_t)fb->width;
    int32_t height = fb->height > g_taskbar_h ? (int32_t)(fb->height-g_taskbar_h) : 0;
    VisibleRect client = visibility_clip(win->x,win->y,win->w,win->h,0,width,height);
    if (!visibility_valid(client)) return;
    region->rects[region->count++] = client;
    bool above = false;
    for (int s = 0; s < g_window_count; ++s) {
        Window* other = g_window_list[g_window_order[s]];
        if (other == win) { above = true; continue; }
        if (!above || !other || !other->visible || !other->buffer) continue;
        VisibleRect cover = visibility_clip(other->x,other->y,other->w,other->h,
                                             (int32_t)g_title_h,width,height);
        if (!visibility_subtract(region,cover)) {
            region->count = 1; region->rects[0] = client;
            return; // Conservative fallback for pathological fragmentation.
        }
        if (!region->count) return;
    }
}

static uint64_t g_rain_render_count = 0;
static uint64_t g_rain_damage_pixels = 0;

// Keep a complete backing surface whenever exposed, but only request repaint
// for pixels people can see. The compositor remains independent of client type.
static inline void wm_rain_render(Window* win, const VisibleRegion& region) {
    if (!win || !win->buffer || !region.count) return;
    uint64_t t0 = rdtsc_val();
    rainbow_render_surface(win->buffer, (uint32_t)win->w, (uint32_t)win->h, win->buffer_pitch, rain_scroll, g_rainbow_wide);
    g_client_render_ticks = rdtsc_val() - t0;
    ++g_rain_render_count;
    if (!win->chrome_drawn) {
        wm_surface_damage_all(win);
        return;
    }
    for (unsigned i = 0; i < region.count; ++i) {
        VisibleRect r = region.rects[i];
        wm_dirty_add(r.x1,r.x2-1,r.y1,r.y2-1);
        g_rain_damage_pixels += (uint64_t)(r.x2-r.x1)*(r.y2-r.y1);
    }
}

// Visual Test as a CLIENT: draws its checkerboard once, then it is just another surface. If the surface
// allocation failed the compositor's direct-draw branch still handles it.
static inline void wm_test_render(Window* win) {
    if (!win || !win->buffer) return;
    checker_render_surface(win->buffer, (uint32_t)win->w, (uint32_t)win->h, win->buffer_pitch);
    wm_surface_damage_all(win);
}

static inline void wm_destroy_window(int id) {
    if (g_focused_win_id == id) {
        g_focused_win_id = -1;
        for (int s = g_window_count - 1; s >= 0; s--) {
            int o_idx = g_window_order[s];
            Window* w = g_window_list[o_idx];
            if (w && w->visible && w->id != id) {
                g_focused_win_id = w->id;
                break;
            }
        }
    }

    int idx = -1;
    for (int i = 0; i < g_window_count; i++) {
        if (g_window_list[i] && g_window_list[i]->id == id) {
            idx = i;
            break;
        }
    }
    if (idx == -1) return;
    
    Window* win = g_window_list[idx];
    if (win->buffer) {
        if (!win->is_system && win->buffer_phys) {
            for (uint64_t p = 0; p < win->buffer_pages; p++) {
                free_phys_page(win->buffer_phys + p * PAGE_SIZE);
            }
        } else {
            kfree(win->buffer);
        }
    }
    kfree(win);
    g_window_list[idx] = nullptr;
    
    for (int i = idx; i < g_window_count - 1; i++) g_window_list[i] = g_window_list[i+1];
    g_window_list[g_window_count - 1] = nullptr;
    
    int order_idx = -1;
    for (int i = 0; i < g_window_count; i++) if (g_window_order[i] == idx) order_idx = i;
    if (order_idx != -1) {
        for (int i = order_idx; i < g_window_count - 1; i++) g_window_order[i] = g_window_order[i+1];
        for (int i = 0; i < g_window_count - 1; i++) if (g_window_order[i] > idx) g_window_order[i]--;
    }
    g_window_count--;
    ++g_resource_demand_generation; g_resources_requested=true;
    
    if (fb) {
        g_dirty_min_x = 0; g_dirty_max_x = fb->width - 1;
        g_dirty_min_y = 0; g_dirty_max_y = fb->height - 1;
    }
    g_needs_refresh = true;
}

static inline void wm_destroy_task_windows(pid_t pid) {
    if (pid == 0) return;
    for (int i = 0; i < g_window_count; i++) {
        Window* win = g_window_list[i];
        if (win && !win->is_system && win->owner_pid == pid) {
            wm_destroy_window(win->id);
            i--;
        }
    }
}

// =============================================================================
//  wm_overlay_update — called on EVERY mouse event (no compositor involved)
//
//  Total pixel cost: ~12x20 cursor + 4x thin outline strips ≈ a few hundred px.
//  This is the entire cursor rendering budget, regardless of window size.
// =============================================================================
volatile bool g_vram_lock = false;

// =============================================================================
//  wm_overlay_update_unlocked
// =============================================================================
static inline void wm_overlay_update_unlocked(int32_t new_cx, int32_t new_cy, uint8_t mb) {
    if (!fb || !g_master_backbuffer) return;

    // Erase old cursor from VRAM
    vram_restore_rect(ov_cx - 1, ov_cy - 1,
                      ov_cx + CURSOR_W + 1, ov_cy + CURSOR_H + 1);

    for (int i = 0; i < g_window_count; i++) {
        Window* win = g_window_list[i];
        if (!win) continue;
        
        int32_t nfx = 0, nfy = 0, nfw = 0, nfh = 0;
        bool dn = win->ghost_dragging;
        if (dn) wm_get_win_rect_from_pos(win, win->ghost_x, win->ghost_y, &nfx, &nfy, &nfw, &nfh);
        
        vram_transition_outline(win->last_ghost_fx, win->last_ghost_fy, win->last_ghost_fw, win->last_ghost_fh, win->last_ghost_active,
                                nfx,     nfy,     nfw,     nfh,     dn);
                                
        win->last_ghost_active = dn;
        if (dn) {
            win->last_ghost_fx = nfx; win->last_ghost_fy = nfy;
            win->last_ghost_fw = nfw; win->last_ghost_fh = nfh;
        }
    }

    // Draw cursor on top of everything
    vram_draw_cursor(new_cx, new_cy, mb);
    ov_cx = new_cx; ov_cy = new_cy;

    vram_fence();
}

static inline void wm_overlay_update(int32_t new_cx, int32_t new_cy, uint8_t mb) {
    while (__sync_lock_test_and_set(&g_vram_lock, 1)) { __asm__ volatile("pause"); }
    wm_overlay_update_unlocked(new_cx, new_cy, mb);
    __sync_lock_release(&g_vram_lock);
}


// =============================================================================
//  wm_init
// =============================================================================
static inline void wm_init() {
    g_window_count = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        g_window_list[i] = nullptr;
        g_window_order[i] = 0;
    }

    uint32_t sn = fb->height;
    uint32_t sd = 1080;

    g_taskbar_h = (sn * 60) / sd;
    g_title_h   = (sn * 42) / sd;
    g_border    = (sn * 3) / sd;
    g_pad       = (sn * 3) / sd;
    
    g_font_scale_100 = (sn >= 2160) ? 200 : (sn >= 1440) ? 150 : 100;
    
    if (g_taskbar_h < 30) g_taskbar_h = 30;
    if (g_title_h < 20)   g_title_h = 20;
    if (g_border < 1)     g_border = 1;
    if (g_pad < 1)        g_pad = 1;
    
    g_sb_w   = (sn * 138) / sd;
    g_wbtn_w = (sn * 186) / sd;
    if (g_sb_w < 80)   g_sb_w = 80;
    if (g_wbtn_w < 110) g_wbtn_w = 110;

    uint32_t desired_test_w = (sn * 800) / sd;
    uint32_t desired_test_h = (sn * 600) / sd;

    g_term_max_cols = desired_test_w / 8;
    g_term_max_rows = desired_test_h / 10;
    if (g_term_max_cols < 40) g_term_max_cols = 40;
    if (g_term_max_rows < 15) g_term_max_rows = 15;

    test_w = g_term_max_cols * 8;
    test_h = g_term_max_rows * 10;
    rain_w = test_w;
    rain_h = test_h;

    g_log_win_cols = g_term_max_cols;
    g_log_rows_visible = g_term_max_rows;

    wm_win_w = test_w;
    wm_win_h = test_h + g_title_h;
    
    wm_win_x = (fb->width > test_w) ? (fb->width - test_w) / 2 : 0;
    wm_win_y = (fb->height > (test_h + g_title_h + g_taskbar_h)) ? (fb->height - (test_h + g_title_h + g_taskbar_h)) / 2 : 0;

    int32_t term_x = (int32_t)wm_win_x;
    int32_t term_y = (int32_t)wm_win_y + (int32_t)g_title_h;

    // Synchronize legacy globals used by terminal.h and framebufferstuff.h
    g_term_ox = term_x;
    g_term_oy = term_y;
    // g_term_max_cols/rows already set above

    // Create initial windows using the unified surface API
    g_win_terminal = wm_create_window("Terminal", term_x, term_y, (int32_t)test_w, (int32_t)test_h, false, true, WIN_TYPE_TERM);
    if (g_win_terminal) {
        g_backbuffer = (uint8_t*)g_win_terminal->buffer;
        memset_32(g_win_terminal->buffer, 0xFF0A0A14, (uint64_t)test_w * test_h);
    }
    g_win_test = wm_create_window("Visual Test", (int32_t)wm_win_x + 50, (int32_t)wm_win_y + 50 + (int32_t)g_title_h, (int32_t)test_w, (int32_t)test_h, true, true, WIN_TYPE_TEST);
    wm_test_render(g_win_test);
    g_win_rain = wm_create_window("Rainbow Animation", (int32_t)wm_win_x + 100, (int32_t)wm_win_y + 100 + (int32_t)g_title_h, (int32_t)test_w, (int32_t)test_h, true, true, WIN_TYPE_RAIN);
    g_win_log = wm_create_window("Boot Log", (int32_t)wm_win_x + 150, (int32_t)wm_win_y + 150 + (int32_t)g_title_h, (int32_t)test_w, (int32_t)test_h, true, true, WIN_TYPE_LOG);

    // Default focus to Terminal and raise it to top of stack
    if (g_win_terminal) {
        wm_raise_window(0); // Terminal is first window created (index 0)
        g_focused_win_id = g_win_terminal->id;
    }

    for (int i = 0; i < 360; i++) {
        g_rainbow_lut[i] = wm_rainbow_color(i * 2);
    }
    for (int j = 0; j < 720; j++) g_rainbow_wide[j] = g_rainbow_lut[j / 2];

    // Boot selects the disk wallpaper before the compositor's first frame.
    // Generate the procedural fallback only if that attempt fails.

    term_dirty_all();
}

// =============================================================================
//  wm_compose_dirty — CURSOR-FREE compositor
//
//  Builds the static scene into master_backbuffer and blits to VRAM.
//  Called by the 60Hz loop ONLY when scene content actually changes
//  (terminal text, scroll, window position committed after drag).
//  The cursor is NOT drawn here; wm_overlay_reapply() handles it after.
// =============================================================================
// Static region to backup pixels under overlays for inclusive staging
static uint32_t g_overlay_backup[256]; 

// Removed old fixed-window helpers


// Composes and flips ONE dirty rect. Returns true if it also ran the cursor/ghost-outline overlay.
// With present=false, prepare the backbuffer only (boot's first desktop frame).
// When several rects are composed in one frame the caller passes do_overlay=true only for the LAST one:
// the overlay draws straight onto VRAM, so a later flip would overwrite what an earlier overlay drew.
static inline bool wm_compose_dirty(uint32_t min_x, uint32_t max_x,
                                     uint32_t min_y, uint32_t max_y, bool do_overlay = true,
                                     bool present = true) {
    if (!fb || !fb->address || !g_backbuffer || !g_master_backbuffer) return false;
    if (min_x >= fb->width)  min_x = 0;
    if (max_x >= fb->width)  max_x = fb->width - 1;
    if (min_y >= fb->height) min_y = 0;
    if (max_y >= fb->height) max_y = fb->height - 1;
    if (min_y > max_y || min_x > max_x) return false;
    ResourceRead resource_read;

    // Chrome/taskbar currently repaint whole horizontal spans on damaged rows.
    // Restore text across those same spans, clipping vertically to avoid blending
    // antialiasing twice onto rows whose backgrounds were not recomposed.
    g_ui_clip_left = 0; g_ui_clip_top = min_y;
    g_ui_clip_right = fb->width; g_ui_clip_bottom = max_y + 1;
    WinRect rects[MAX_WINDOWS];
    for (int i = 0; i < g_window_count; i++) {
        if (g_window_list[i]) wm_get_win_rect(g_window_list[i], &rects[i]);
    }
    
    // Metrics reset (B/W/r/L are updated DURING the frame, V is from last)
    g_time_bg_ticks   = 0;
    g_time_win_ticks  = 0;
    g_time_rain_ticks = 0;
    g_time_lock_ticks = 0;

    uint64_t t0 = rdtsc_val();
    uint64_t t1, t2, t3;

    // Stage 1: Desktop wallpaper (hole-punched)
    for (uint32_t y = min_y; y <= max_y; y++) {
        uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)y * fb->pitch);
        uint32_t* wrow = nullptr;
        if (g_wallpaper && y < fb->height) {
            wrow = g_wallpaper + (uint64_t)y * fb->width;
        }
        if (y >= fb->height - g_taskbar_h) continue; // Taskbar area

        // Collect opaque spans for this scanline
        struct Span { int32_t x1, x2; };
        Span spans[MAX_WINDOWS];
        int num_spans = 0;
        
        for (int i = 0; i < g_window_count; i++) {
            if (g_window_list[i] && rects[i].visible && (int32_t)y >= rects[i].y1 && (int32_t)y < rects[i].y2) {
                // Intersects this scanline
                int32_t sx1 = rects[i].x1; if (sx1 < (int32_t)min_x) sx1 = min_x;
                int32_t sx2 = rects[i].x2; if (sx2 > (int32_t)max_x + 1) sx2 = max_x + 1;
                if (sx1 < sx2) {
                    spans[num_spans].x1 = sx1;
                    spans[num_spans].x2 = sx2;
                    num_spans++;
                }
            }
        }
        
        // Sort spans by x1
        for (int i = 1; i < num_spans; i++) {
            Span key = spans[i];
            int j = i - 1;
            while (j >= 0 && spans[j].x1 > key.x1) {
                spans[j + 1] = spans[j];
                j--;
            }
            spans[j + 1] = key;
        }

        // Draw the gaps between spans (desktop background)
        int32_t current_x = min_x;
        for (int i = 0; i < num_spans; i++) {
            if (current_x < spans[i].x1) {
                int32_t gap = spans[i].x1 - current_x;
                if (gap > 0) {
                    if (wrow) {
                        memcpy_vram_sse_headless(
                            (uint8_t*)(drow + current_x),
                            (uint8_t*)(wrow + current_x),
                            (uint32_t)gap * 4
                        );
                    } else {
                        memset_32(drow + current_x, 0xFF008080, gap);
                    }
                }
            }
            if (current_x < spans[i].x2) {
                current_x = spans[i].x2;
            }
        }
        
        // Draw any remaining background after the last span
        if (current_x <= (int32_t)max_x) {
            uint32_t gap = max_x - (uint32_t)current_x + 1;
            if (gap > 0) {
                if (wrow) {
                    memcpy_vram_sse_headless(
                        (uint8_t*)(drow + current_x),
                        (uint8_t*)(wrow + current_x),
                        gap * 4
                    );
                } else {
                    memset_32(drow + current_x, 0xFF008080, gap);
                }
            }
        }
    }
    
    t1 = rdtsc_val();
    g_time_bg_ticks = t1 - t0;

    // Stage 2: Windows in stack order
    for (int s = 0; s < g_window_count; s++) {
        int idx = g_window_order[s];
        Window* win = g_window_list[idx];
        if (!win || !win->visible) continue;
        WinRect r = rects[idx];

        int32_t dr1 = (r.y1 > (int32_t)min_y) ? r.y1 : (int32_t)min_y;
        int32_t dr2 = (r.y2 < (int32_t)max_y) ? r.y2 : (int32_t)max_y;
        // Don't draw windows over taskbar area
        if (dr2 > (int32_t)(fb->height - g_taskbar_h)) dr2 = fb->height - g_taskbar_h;
        if (dr1 >= dr2) continue;

        // Unified surface compositor: every window (system or user) is backed by its surface buffer
        wm_draw_window_chrome(r.x1, r.y1, r.x2 - r.x1, r.y2 - r.y1, win->title, win->closable, min_y, max_y, win->id == g_focused_win_id, &wm_window_icons(win).title);
        if (win->buffer) {
            for (int32_t y = dr1; y < dr2; y++) {
                if (y < r.y1 + (int32_t)g_title_h) continue;
                uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)y * fb->pitch);
                int32_t ry = y - (r.y1 + (int32_t)g_title_h);
                int32_t wx = r.x1; if (wx < 0) wx = 0;
                int32_t ww = (r.x2 - r.x1); if (r.x1 < 0) ww += r.x1;
                if (wx + ww > (int32_t)fb->width) ww = (int32_t)fb->width - wx;
                if (ww > 0) {
                    uint32_t src_off = (r.x1 < 0) ? (uint32_t)(-r.x1) : 0;
                    memcpy_vram_sse_headless(drow + wx, (uint32_t*)((uint8_t*)win->buffer + (uint64_t)ry * win->buffer_pitch) + src_off, ww * 4);
                }
            }
        }
    }

    // Stage 2.5: Task Bar — Modern dark glass style
    int32_t tb_y = (int32_t)fb->height - (int32_t)g_taskbar_h;
    int32_t tbr1 = (tb_y > (int32_t)min_y) ? tb_y : (int32_t)min_y;
    int32_t tbr2 = (int32_t)max_y;
    if (tbr1 <= tbr2) {

        // ── Background gradient (deep navy, lighter at top) ──────────────────
        for (int32_t y = tbr1; y <= tbr2; y++) {
            if ((uint32_t)y >= fb->height) break;
            uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)y * fb->pitch);
            uint32_t row_off = (uint32_t)(y - tb_y);
            uint32_t clr;
            if      (row_off == 0) clr = 0xFF5a6490;   // bright shelf highlight
            else if (row_off == 1) clr = 0xFF303555;   // sub-highlight
            else {
                uint32_t t = ((row_off - 2) * 255) / (g_taskbar_h > 4 ? g_taskbar_h - 3 : 1);
                uint8_t r  = (uint8_t)(0x1c + (t * 4) / 255);
                uint8_t gc = (uint8_t)(0x20 + (t * 3) / 255);
                uint8_t bc = (uint8_t)(0x38 + (t * 5) / 255);
                clr = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)gc << 8) | bc;
            }
            memset_32(drow, clr, fb->width);
        }

        // ── Helper: gradient-filled button rect ─────────────────────────────
        // top_c/bot_c are the gradient endpoints; edge_c is the 1px border;
        // shine=true draws a bright highlight row on top (glass effect).
        auto draw_btn = [&](int32_t bx, int32_t by, int32_t bw, int32_t bh,
                            uint32_t top_c, uint32_t bot_c,
                            uint32_t edge_c, bool shine) {
            for (int32_t y = by; y < by + bh; y++) {
                if (y < (int32_t)min_y || y > (int32_t)max_y) continue;
                if (y < 0 || (uint32_t)y >= fb->height) continue;
                uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)y * fb->pitch);
                uint32_t t = (uint32_t)(y - by) * 255 / (bh > 1 ? (uint32_t)(bh - 1) : 1u);
                // Lerp each channel
                auto lerp8 = [](uint32_t a, uint32_t b, uint32_t t) -> uint8_t {
                    return (uint8_t)((int32_t)(a >> 16 & 0xFF) +
                        ((int32_t)(b >> 16 & 0xFF) - (int32_t)(a >> 16 & 0xFF)) * (int32_t)t / 255);
                };
                uint8_t rr  = (uint8_t)(((top_c>>16)&0xFF) + (int32_t)(((int32_t)((bot_c>>16)&0xFF)-(int32_t)((top_c>>16)&0xFF))*(int32_t)t/255));
                uint8_t gg  = (uint8_t)(((top_c>> 8)&0xFF) + (int32_t)(((int32_t)((bot_c>> 8)&0xFF)-(int32_t)((top_c>> 8)&0xFF))*(int32_t)t/255));
                uint8_t bb  = (uint8_t)(((top_c    )&0xFF) + (int32_t)(((int32_t)((bot_c    )&0xFF)-(int32_t)((top_c    )&0xFF))*(int32_t)t/255));
                uint32_t row_clr = 0xFF000000 | ((uint32_t)rr<<16) | ((uint32_t)gg<<8) | bb;
                if (shine && y == by) row_clr = 0xFFAABBEE;  // top shine row

                int32_t sx = bx < 0 ? 0 : bx;
                int32_t ex = bx + bw; if (ex > (int32_t)fb->width) ex = (int32_t)fb->width;
                if (ex > sx) memset_32(drow + sx, row_clr, (uint32_t)(ex - sx));

                // Left/right 1px edges
                if (bx >= 0 && (uint32_t)bx < fb->width)           drow[bx]       = edge_c;
                if (bx+bw-1 >= 0 && (uint32_t)(bx+bw-1) < fb->width) drow[bx+bw-1] = edge_c;
            }
            // Top / bottom edge lines
            for (int32_t px = bx; px < bx + bw; px++) {
                if (px < 0 || (uint32_t)px >= fb->width) continue;
                auto edge_px = [&](int32_t ey, uint32_t ec) {
                    if (ey < (int32_t)min_y || ey > (int32_t)max_y) return;
                    if (ey < 0 || (uint32_t)ey >= fb->height) return;
                    ((uint32_t*)(g_master_backbuffer + (uint64_t)ey * fb->pitch))[px] = ec;
                };
                edge_px(by,        edge_c);
                edge_px(by + bh - 1, 0xFF101828);
            }
        };

        // ── Start button ─────────────────────────────────────────────────────
        const int32_t PAD = 5;
        int32_t sb_x = 6,   sb_y = tb_y + PAD;
        int32_t sb_h = (int32_t)g_taskbar_h - PAD * 2;

        draw_btn(sb_x, sb_y, g_sb_w, sb_h,
                0xFF4a7ad8, 0xFF2a50b0,   // blue gradient
                0xFF1a3a90,               // dark blue border
                true);

        // "# START" label, centered
        const char* start_lbl   = "# START";
        uint32_t char_h = wm_text_height(g_font_scale_100);
        wm_draw_string(sb_x + ((int32_t)g_sb_w - wm_text_width(start_lbl, g_font_scale_100)) / 2,
                    sb_y + (int32_t)(sb_h - char_h) / 2,
                    start_lbl, 0xFFFFFFFF, g_font_scale_100);
        
        // ── System tray (right side, drawn before task buttons so we know its x) 
        const int32_t TRAY_W = wm_tray_w();
        int32_t tray_x = (int32_t)fb->width - TRAY_W - 6;
        int32_t tray_y = sb_y, tray_h = sb_h;

        draw_btn(tray_x, tray_y, TRAY_W, tray_h,
                0xFF252a42, 0xFF1a1f35,
                0xFF303560, false);

        // Clock: time on top, date below (like the Windows taskbar). Only reads the cached g_clock.
        {
            char tbuf[16], dbuf[16];
            int tl, dl = 0;
            if (g_clock_valid) {
                tl = rtc_format_time(g_clock, tbuf, CLOCK_SHOW_SECONDS != 0);
                dl = rtc_format_date(g_clock, dbuf);
            } else {
                tbuf[0] = '-'; tbuf[1] = '-'; tbuf[2] = ':'; tbuf[3] = '-'; tbuf[4] = '-'; tbuf[5] = 0;
                tl = 5;
            }
            uint32_t clock_scale = wm_clock_scale();
            int32_t ch = wm_text_height(clock_scale);
            bool two_lines = dl > 0 && tray_h >= 2 * ch + 2;
            int32_t ty0 = two_lines ? tray_y + (tray_h - (2 * ch + 2)) / 2
                                    : tray_y + (tray_h - ch) / 2;
            wm_draw_string((uint32_t)(tray_x + (TRAY_W - wm_text_width(tbuf, clock_scale)) / 2), (uint32_t)ty0,
                           tbuf, 0xFFE6E8FF, clock_scale);
            if (two_lines)
                wm_draw_string((uint32_t)(tray_x + (TRAY_W - wm_text_width(dbuf, clock_scale)) / 2), (uint32_t)(ty0 + ch + 2),
                               dbuf, 0xFF9FA6CC, clock_scale);
        }

        // Separator between task area and tray
        for (int32_t y = sb_y + 4; y < sb_y + sb_h - 4; y++) {
            if (y < (int32_t)min_y || y > (int32_t)max_y) continue;
            if (y < 0 || (uint32_t)y >= fb->height) continue;
            uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)y * fb->pitch);
            int32_t sep = tray_x - 6;
            if (sep >= 0 && (uint32_t)(sep+1) < fb->width) {
                drow[sep]   = 0xFF404870;
                drow[sep+1] = 0xFF1a1f35;
            }
        }

        // ── Window task buttons ───────────────────────────────────────────────
        const int32_t WBTN_GAP = (int32_t)(4 * fb->height) / 1080;
        int32_t cur_x = sb_x + g_sb_w + (int32_t)(10 * fb->height) / 1080;
        int32_t active_idx = (g_window_count > 0) ? g_window_order[g_window_count - 1] : -1;

        for (int idx = 0; idx < g_window_count; idx++) {
            Window* win = g_window_list[idx];
            if (!win || !win->visible) continue;
            bool active = (win->id == g_focused_win_id);

            uint32_t top_c  = active ? 0xFF3d4d80 : 0xFF23283d;
            uint32_t bot_c  = active ? 0xFF28366a : 0xFF181c30;
            uint32_t edge_c = active ? 0xFF5568b0 : 0xFF2c3258;

            draw_btn(cur_x, sb_y, g_wbtn_w, sb_h, top_c, bot_c, edge_c, active);

            if (active) {
                int32_t bar_y = sb_y;
                if (bar_y >= (int32_t)min_y && bar_y <= (int32_t)max_y &&
                    bar_y >= 0 && (uint32_t)bar_y < fb->height) {
                    uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)bar_y * fb->pitch);
                    int32_t bar_x1 = cur_x + 2, bar_x2 = cur_x + g_wbtn_w - 3;
                    if (bar_x1 < 0) bar_x1 = 0;
                    if (bar_x2 >= (int32_t)fb->width) bar_x2 = (int32_t)fb->width - 1;
                    if (bar_x2 > bar_x1) memset_32(drow + bar_x1, 0xFF6688FF, (uint32_t)(bar_x2 - bar_x1));
                }
            }

            const Image& task_icon=wm_window_icons(win).task;
            int32_t text_offset=8;
            if (task_icon.pixels) {
                wm_draw_icon(task_icon,cur_x+6,sb_y+((int32_t)sb_h-(int32_t)task_icon.height)/2,
                             cur_x,min_y,cur_x+(int32_t)g_wbtn_w,max_y+1);
                text_offset=12+task_icon.width;
            }
            wm_draw_string(cur_x + text_offset, sb_y + (int32_t)(sb_h - char_h) / 2,
                        win->title,
                        active ? 0xFFEEEEFF : 0xFF8888AA,
                        g_font_scale_100, g_wbtn_w - text_offset - 8);

            cur_x += g_wbtn_w + WBTN_GAP;
        }
    }

    // Stage 2.6: RAM display (top-right)
    {
        uint64_t total_mb = get_total_ram_count() / (1024 * 1024);
        uint64_t mapped_mb = get_mapped_ram_count() / (1024 * 1024);
        char m_str[16];
        char t_str[16];
        char ram_msg[64];
        
        to_str(mapped_mb, m_str);
        to_str(total_mb, t_str);
        
        char* d = ram_msg;
        const char* p;
        p = "RAM: "; while(*p) *d++ = *p++;
        p = m_str;   while(*p) *d++ = *p++;
        p = " MiB / "; while(*p) *d++ = *p++;
        p = t_str;   while(*p) *d++ = *p++;
        p = " MiB";   while(*p) *d++ = *p++;
        *d = '\0';
        
        uint32_t msg_len = strlen(ram_msg);
        uint32_t msg_px = msg_len * 8 + 20;
        uint32_t rx = (fb->width > msg_px) ? fb->width - msg_px : 0;
        uint32_t ry = 20;
        
        // Draw background only if within dirty rect
        if (ry + 12 >= min_y && ry - 4 <= max_y) {
            for (uint32_t by = ry - 6; by < ry + 14; by++) {
                if (by < min_y || by > max_y) continue;
                if (by >= fb->height) break;
                uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)by * fb->pitch);
                uint32_t bx = (rx > 6) ? rx - 6 : 0;
                uint32_t bw = msg_len * 8 + 12;
                if (bx + bw > fb->width) bw = fb->width - bx;
                memset_32(drow + bx, 0xCC1A1F35, bw); // Dark navy glass
            }
            wm_draw_mono_string(rx, ry, ram_msg, 0xFFEEEEFF);
        }
    }

    // Stage 2.7: CPU display (below RAM)
    {
        // ── Task Count measurement ──────────────────────────────────────────
        static uint32_t g_task_count = 0;
        static uint64_t last_count_tick = 0;
        if (g_tick_count > last_count_tick + 500) { // Update tasks every 500ms
            last_count_tick = g_tick_count;
            uint32_t count = 0;
            extern Task* g_task_list;
            if (g_task_list) {
                Task* t = g_task_list;
                do {
                    count++;
                    t = t->next;
                } while (t != g_task_list);
            }
            g_task_count = count;
        }

        // ── CPU usage via TSC idle measurement ───────────────────────────────
        static CpuUsageSample usage;

        uint64_t now_tsc;
        __asm__ volatile("rdtsc; shl $32, %%rdx; or %%rdx, %%rax" : "=a"(now_tsc) :: "rdx");

        usage.update(now_tsc,g_idle_tsc_accum,tsc_hz);
        uint32_t g_cpu_usage_pct=usage.percent;

        // ── Extended Topology core count (Level 0xB) ────────────────────────
        static uint32_t g_vthr = 0, g_cores = 0;
        if (g_vthr == 0) {
            uint32_t eax, ebx, ecx, edx;
            __asm__ volatile("cpuid" : "=a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx) : "a"(0), "c"(0));
            uint32_t max_leaf = eax;
            bool got_topo = false;
            if (max_leaf >= 0xB) {
                __asm__ volatile("cpuid" : "=a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx) : "a"(0xB), "c"(0));
                uint32_t th_p_core = ebx & 0xFFFF;
                __asm__ volatile("cpuid" : "=a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx) : "a"(0xB), "c"(1));
                uint32_t tot_log = ebx & 0xFFFF;
                if (tot_log > 0 && th_p_core > 0) {
                    g_vthr = tot_log; g_cores = tot_log / th_p_core;
                    got_topo = true;
                }
            }
            if (!got_topo) {
                __asm__ volatile("cpuid" : "=a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx) : "a"(1), "c"(0));
                g_vthr = (ebx >> 16) & 0xFF; if (g_vthr == 0) g_vthr = 1;
                g_cores = g_vthr;
            }
        }

        // ── Task states & Frame time ──────────────────────────────────────────────
        char states_buf[160];
        char* sp = states_buf;
        *sp++ = ' '; *sp++ = '|'; *sp++ = ' ';
        if (g_task_list) {
            Task* t = g_task_list;
            int limit = 4;
            do {
                to_str(t->pid, sp); while(*sp) sp++; // append PID
                *sp++ = ':';
                if (t->state == TaskState::READY) *sp++ = 'R'; 
                else if (t->state == TaskState::BLOCKED) *sp++ = 'B';
                else if (t->state == TaskState::DEAD) *sp++ = 'D';
                else if (t->state == TaskState::RUNNING) *sp++ = 'H';
                else *sp++ = '?';
                *sp++ = ' ';
                t = t->next;
            } while (t != g_task_list && --limit > 0);
        }
        
        *sp++ = '|'; *sp++ = ' '; *sp++ = 'F'; *sp++ = ':';
        sp = hud_put_ms1(sp, g_last_frame_ticks);
        *sp++ = 'm'; *sp++ = 's';
        *sp++ = ' '; *sp++ = '('; *sp++ = 'B'; *sp++ = ':'; sp = hud_put_ms1(sp, g_snap_bg);
        *sp++ = ' '; *sp++ = 'W'; *sp++ = ':'; sp = hud_put_ms1(sp, g_snap_win);
        *sp++ = ' '; *sp++ = 'r'; *sp++ = ':'; sp = hud_put_ms1(sp, g_snap_rain);
        *sp++ = ' '; *sp++ = 'L'; *sp++ = ':'; sp = hud_put_ms1(sp, g_snap_lock);
        *sp++ = ' '; *sp++ = 'V'; *sp++ = ':'; sp = hud_put_ms1(sp, g_snap_vram);
        {   // X = time inside wm_compose_dirty() that none of the B/W/V stage timers cover
            uint64_t acc = g_snap_bg + g_snap_win + g_snap_vram;
            uint64_t other = g_last_frame_ticks > acc ? g_last_frame_ticks - acc : 0;
            *sp++ = ' '; *sp++ = 'X'; *sp++ = ':'; sp = hud_put_ms1(sp, other);
        }
        *sp++ = ')'; *sp++ = ' ';
        *sp = '\0';

        // ── Build HUD string ────────────────────────────────────────────────
        char cpu_msg[HUD_MSG_CAP];
        char p1_str[8], tc_str[8], ut_str[8];
        to_str(g_cpu_usage_pct, p1_str);
        to_str(g_task_count, tc_str);
        to_str(g_tick_count / 1000, ut_str); 

        char* d = cpu_msg; const char* p;
        p = "Core0: "; while(*p) *d++ = *p++;
        p = p1_str;   while(*p) *d++ = *p++;
        p = "%";      while(*p) *d++ = *p++;
        p = states_buf; while(*p) *d++ = *p++;
        p = " | T:";   while(*p) *d++ = *p++;
        p = tc_str;   while(*p) *d++ = *p++;
        p = " | Up:";  while(*p) *d++ = *p++;
        p = ut_str;   while(*p) *d++ = *p++;
        p = "s";      while(*p) *d++ = *p++;
        p = " | ";    while(*p) *d++ = *p++;
        p = g_gpu_status; while(*p && (d - cpu_msg) < (HUD_MSG_CAP - 2)) *d++ = *p++;
        *d = '\0';

        uint32_t msg_len = strlen(cpu_msg);
        uint32_t rx = fb->width - (msg_len * 8) - 20;
        uint32_t ry = 38; 

        if ((uint32_t)(ry + 12) >= min_y && ry >= 4 && (ry - 4) <= max_y) {
            for (uint32_t by = ry - 4; by < ry + 12; by++) {
                if (by < min_y || by > max_y) continue;
                if (by >= fb->height) break;
                uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)by * fb->pitch);
                uint32_t bx = (rx > 6) ? rx - 6 : 0;
                uint32_t bw = msg_len * 8 + 12;
                if (bx + bw > fb->width) bw = fb->width - bx;
                memset_32(drow + bx, 0xCC1A1F35, bw);
            }
            // Color: green when low usage, yellow when moderate, red when high
            uint32_t cpu_color = g_cpu_usage_pct < 50 ? 0xFF88FF88 :
                                 g_cpu_usage_pct < 80 ? 0xFFFFDD44 : 0xFFFF6644;
            wm_draw_mono_string(rx, ry, cpu_msg, cpu_color);
        }
    }
    
    // Stage 2.8: Start menu popup (topmost scene layer, below the cursor overlay)
    if (g_menu_open && fb) {
        int32_t px, py, pw, ph;
        wm_menu_geom(&px, &py, &pw, &ph);

        uint32_t sn = fb->height, sd = 1080;
        int32_t hdr_h = (int32_t)((sn * 36) / sd); if (hdr_h < 24) hdr_h = 24;
        int32_t row_h = (int32_t)((sn * 38) / sd); if (row_h < 26) row_h = 26;
        int32_t pad   = (int32_t)((sn * 10) / sd); if (pad < 6) pad = 6;

        // Hover index comes straight from the live cursor position — no state to keep in sync.
        int hover = wm_menu_hit_test(g_mouse_x, g_mouse_y);

        uint32_t char_h = wm_text_height(g_font_scale_100);

        // ── Panel body: dark navy glass, same family as the taskbar
        for (int32_t y = py; y < py + ph; y++) {
            if (y < (int32_t)min_y || y > (int32_t)max_y) continue;
            if (y < 0 || (uint32_t)y >= fb->height) continue;
            uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)y * fb->pitch);

            int32_t rel = y - py;
            uint32_t bgc;
            if (rel == 0) {
                bgc = 0xFF6A76A8; // top shelf highlight, matches the taskbar's bright edge
            } else if (rel < hdr_h) {
                uint32_t t = (uint32_t)(rel - 1) * 255 / (hdr_h > 2 ? (uint32_t)(hdr_h - 2) : 1u);
                int32_t rr = (int32_t)0x2E + ((int32_t)0x23 - (int32_t)0x2E) * (int32_t)t / 255;
                int32_t gg = (int32_t)0x46 + ((int32_t)0x2F - (int32_t)0x46) * (int32_t)t / 255;
                int32_t bb = (int32_t)0x70 + ((int32_t)0x52 - (int32_t)0x70) * (int32_t)t / 255;
                bgc = 0xFF000000 | ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | (uint32_t)bb;
            } else if (rel == hdr_h) {
                bgc = 0xFF0D1120; // separator line under the header
            } else {
                bgc = 0xFF161B2C; // body
            }

            int32_t sx = px < 0 ? 0 : px;
            int32_t ex = px + pw; if (ex > (int32_t)fb->width) ex = (int32_t)fb->width;
            if (ex > sx) memset_32(drow + sx, bgc, (uint32_t)(ex - sx));
        }

        // ── Header text: "START" left, app count right
        {
            int32_t hy = py + (hdr_h - (int32_t)char_h) / 2;
            if (hy + (int32_t)char_h >= (int32_t)min_y && hy <= (int32_t)max_y) {
                wm_draw_string((uint32_t)(px + pad), (uint32_t)hy, "START", 0xFFFFFFFF, g_font_scale_100);
                char cnt[16];
                to_str((uint64_t)g_menu_app_count, cnt);
                wm_draw_string((uint32_t)(px + pw - pad - wm_text_width(cnt, g_font_scale_100)), (uint32_t)hy, cnt, 0xFF9FB0D8, g_font_scale_100);
            }
        }

        // ── App list: icon + name per row, hover highlight like an active taskbar button
        int32_t list_top = py + hdr_h + pad;
        for (int i = 0; i < g_menu_app_count; i++) {
            int32_t ry = list_top + i * row_h;
            if (ry >= py + ph - pad) break; // clipped by the height cap

            bool hov = (i == hover);

            if (hov) {
                for (int32_t y = ry; y < ry + row_h; y++) {
                    if (y < (int32_t)min_y || y > (int32_t)max_y) continue;
                    if (y < 0 || (uint32_t)y >= fb->height) continue;
                    uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)y * fb->pitch);
                    uint32_t t = (uint32_t)(y - ry) * 255 / (row_h > 1 ? (uint32_t)(row_h - 1) : 1u);
                    int32_t rr = (int32_t)0x3D + ((int32_t)0x28 - (int32_t)0x3D) * (int32_t)t / 255;
                    int32_t gg = (int32_t)0x4D + ((int32_t)0x36 - (int32_t)0x4D) * (int32_t)t / 255;
                    int32_t bb = (int32_t)0x80 + ((int32_t)0x6A - (int32_t)0x80) * (int32_t)t / 255;
                    uint32_t c = 0xFF000000 | ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | (uint32_t)bb;
                    int32_t sx = px + pad < 0 ? 0 : px + pad;
                    int32_t ex = px + pw - pad; if (ex > (int32_t)fb->width) ex = (int32_t)fb->width;
                    if (ex > sx) memset_32(drow + sx, c, (uint32_t)(ex - sx));
                }
            }

            const auto& entry=g_resources_catalog->apps[i];
            uint32_t hash = 0;
            for (const char* q = entry.app.id; *q; q++) hash = hash * 31u + (uint8_t)*q;
            uint32_t icon_c = g_rainbow_lut[hash % 360];
            int32_t isz = wm_icon_size(row_h-10,14,128);
            int32_t ix = px + pad, iy = ry + (row_h - isz) / 2;
            if (entry.icons.menu.pixels) {
                const Image& icon=entry.icons.menu;
                wm_draw_icon(icon,ix+(isz-(int32_t)icon.width)/2,iy+(isz-(int32_t)icon.height)/2,
                             px+pad,min_y,px+pw-pad,max_y+1);
            } else for (int32_t y = iy; y < iy + isz; y++) {
                if (y < (int32_t)min_y || y > (int32_t)max_y) continue;
                if (y < 0 || (uint32_t)y >= fb->height) continue;
                uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)y * fb->pitch);
                int32_t sx = ix < 0 ? 0 : ix;
                int32_t ex = ix + isz; if (ex > (int32_t)fb->width) ex = (int32_t)fb->width;
                if (ex > sx) memset_32(drow + sx, icon_c, (uint32_t)(ex - sx));
            }

            uint32_t txt_clr = hov ? 0xFFFFFFFF : 0xFFE4E8F5;
            if (!entry.app.available) txt_clr = 0xFF6A7088; // pinned app whose file/drive is missing
            int32_t ty = ry + (row_h - (int32_t)char_h) / 2;
            if (ty + (int32_t)char_h >= (int32_t)min_y && ty <= (int32_t)max_y)
                wm_draw_string((uint32_t)(ix + isz + 8), (uint32_t)ty, entry.app.name, txt_clr, g_font_scale_100, px + pw - pad - (ix + isz + 8));
        }

        // Empty state
        if (g_menu_app_count == 0 && list_top < py + ph - pad) {
            int32_t ty = list_top + (row_h - (int32_t)char_h) / 2;
            if (ty + (int32_t)char_h >= (int32_t)min_y && ty <= (int32_t)max_y)
                wm_draw_string((uint32_t)(px + pad), (uint32_t)ty, "No installed apps", 0xFF8890A8, g_font_scale_100);
        }
    }

    t2 = rdtsc_val();
    g_time_win_ticks = t2 - t1;

    // Boot keeps the splash in VRAM until every part of the desktop is ready.
    // No cursor, overlay, GPU hook, or framebuffer write may precede this gate.
    if (!present) return false;

    // Stage 3: Blit dirty rectangle to VRAM
    if (min_x > max_x || min_y > max_y) {
        vram_fence();
        return false;
    }
    uint32_t ptch = fb->pitch;
    uint32_t bw   = (max_x - min_x + 1) * 4;

    uint64_t lt0 = rdtsc_val();
    while (__sync_lock_test_and_set(&g_vram_lock, 1)) { __asm__ volatile("pause"); }
    g_time_lock_ticks = rdtsc_val() - lt0;

    // ── Compose cursor into master_backbuffer before blit ──────────────────
    // This ensures the cursor is ALWAYS included in the blit and there is
    // never a frame where the cursor is missing from VRAM.
    // We save the affected pixels first so we can restore backbuffer after.
    int32_t ccx = g_mouse_x, ccy = g_mouse_y;
    uint8_t cmb = g_mouse_buttons;
    static uint32_t cursor_save_buf[CURSOR_H * CURSOR_W];
    uint32_t pitch32 = ptch / 4;
    for (int cr = 0; cr < CURSOR_H; cr++) {
        int32_t sy = ccy + cr;
        if (sy < 0 || (uint32_t)sy >= fb->height) {
            for (int x = 0; x < CURSOR_W; x++) cursor_save_buf[cr * CURSOR_W + x] = 0;
            continue;
        }
        uint32_t* bbrow = (uint32_t*)(g_master_backbuffer + (uint64_t)sy * ptch);
        for (int x = 0; x < CURSOR_W; x++) {
            int32_t sx = ccx + x;
            if (sx < 0 || (uint32_t)sx >= fb->width) { cursor_save_buf[cr * CURSOR_W + x] = 0; continue; }
            cursor_save_buf[cr * CURSOR_W + x] = bbrow[sx]; // save
            uint8_t p = g_cursor_bitmap[cr][x];
            if (p) bbrow[sx] = (p == 2)
                ? ((cmb & 1) ? 0xFFFF0000 : (cmb & 2) ? 0xFF00FFFF : 0xFF000000)
                : 0xFFFFFFFF; // stamp
        }
    }

    // ── Blit dirty rect (64-byte aligned for max throughput) ───────────────
    uint32_t min_x_aligned = min_x & ~15u; // 16-byte align for SSE (64 would be safer but might over-copy too much)
    uint32_t max_x_aligned = (max_x + 15) & ~15u;
    if (max_x_aligned > fb->width) max_x_aligned = fb->width;
    uint32_t bw_aligned = (max_x_aligned - min_x_aligned) * 4;

    // GPU path: one blitter copy of the whole dirty rect (synchronous: returns after the
    // fence, so the cursor restore below cannot race the blit). Falls back to the CPU
    // loop if the hook is unset or the GPU reported a problem.
    bool gpu_done = false;
    if (g_gpu_flip_hook) gpu_done = g_gpu_flip_hook(min_x_aligned, min_y, max_x_aligned, max_y + 1);
    if (!gpu_done) {
        for (uint32_t y = min_y; y <= max_y; y++) {
            uint8_t* dst = (uint8_t*)fb->address + (uint64_t)y * ptch + (uint64_t)min_x_aligned * 4;
            uint8_t* src = g_master_backbuffer   + (uint64_t)y * ptch + (uint64_t)min_x_aligned * 4;
            memcpy_vram_sse_headless(dst, src, bw_aligned);
        }
    }
    __asm__ volatile("sfence" : : : "memory");
    t3 = rdtsc_val();
    
    g_time_bg_ticks   = t1 - t0;
    g_time_win_ticks  = t2 - t1;
    g_time_vram_ticks = t3 - t2;
    g_snap_bg = g_time_bg_ticks; g_snap_win = g_time_win_ticks; g_snap_rain = g_time_rain_ticks + g_client_render_ticks;
    g_snap_lock = g_time_lock_ticks; g_snap_vram = g_time_vram_ticks;

    // ── Restore backbuffer cursor pixels ────────────────────────────────────
    // (master_backbuffer does not own cursor pixels; VRAM does)
    for (int cr = 0; cr < CURSOR_H; cr++) {
        int32_t sy = ccy + cr;
        if (sy < 0 || (uint32_t)sy >= fb->height) continue;
        uint32_t* bbrow = (uint32_t*)(g_master_backbuffer + (uint64_t)sy * ptch);
        for (int x = 0; x < CURSOR_W; x++) {
            int32_t sx = ccx + x;
            if (sx < 0 || (uint32_t)sx >= fb->width) continue;
            if (g_cursor_bitmap[cr][x]) bbrow[sx] = cursor_save_buf[cr * CURSOR_W + x];
        }
    }

    // Draw ghost outlines to VRAM and update cursor position tracking.
    // Ghost outlines are direct-VRAM-only (not baked into backbuffer).
    // NOTE: do NOT set ov_cx/ov_cy here — wm_overlay_update_unlocked needs
    // the OLD values to erase the previous cursor position from VRAM.
    if (do_overlay) wm_overlay_update_unlocked(ccx, ccy, cmb);

    __sync_lock_release(&g_vram_lock);
    return do_overlay;
}

#endif // WM_H
