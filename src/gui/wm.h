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
#include "gpu_compositor.h"
#include "gpu_deferred_alpha.h"
#include "gpu_occlusion_metrics.h"
#include "window_alpha.h"
#include "alpha_renderer.h"
#include "window_shadow.h"
#include "title_bar.h"
#include "gpu_presentation.h"
#include "../core/frame_gap_trace.h"

#include "font8x8_basic.h"
#include "ui_font.h"
#include "truetype.h"
#include "resources.h"
#include "window_resize.h"
#include "window_input.h"
#include "window_decorations.h"
#include "window_overlay.h"
#include "overlay_renderer.h"
#include "power_icons.h"
#include "volume_popup.h"
#include "../drivers/audio_volume.h"
#include "clipboard_icons.h"
#include "text_clipboard.h"

void* image_allocate(uint64_t size) { return kmalloc(size); }
void image_deallocate(void* pointer) { kfree(pointer); }

static resources::Catalog g_empty_catalog{};
static resources::Catalog* g_resources_catalog = &g_empty_catalog;
static uint64_t g_decoration_epoch=1;
static volatile bool g_resources_requested = true;
static volatile bool g_resources_refresh = true;
static uint64_t g_resource_demand_generation = 0;
static uint64_t g_resource_refresh_ticks = 0;
static unsigned g_resource_readers = 0;
// Publication waits for readers to finish. Readers never wait or touch disk.
struct ResourceRead {
    ResourceRead() { __atomic_add_fetch(&g_resource_readers, 1u, __ATOMIC_ACQUIRE); }
    ~ResourceRead() { __atomic_sub_fetch(&g_resource_readers, 1u, __ATOMIC_RELEASE); }
};
static inline uint64_t rdtsc_val();
extern uint64_t tsc_hz;
static bool g_resource_cooperative=false;
static uint64_t g_resource_slice_started=0;
static void wm_resource_checkpoint(){
    if(!g_resource_cooperative || !tsc_hz)return;
    uint64_t now=rdtsc_val();
    if(now-g_resource_slice_started<tsc_hz/2000)return;
    task_sleep_ms(1);
    g_resource_slice_started=rdtsc_val();
}

static uint8_t* wm_resource_read(const char* path, uint64_t limit, uint64_t* size) {
    wm_resource_checkpoint();
    file f{}; *size=0;
    if (vfs_open(path,0,&f)!=0) return nullptr;
    uint64_t count=f.vn->size;
    uint8_t* data=f.vn->type==VnodeType::FILE && count && count<=limit ? (uint8_t*)kmalloc(count) : nullptr;
    uint64_t loaded=0,started=rdtsc_val();
    while (data && loaded<count && rdtsc_val()-started<_usb_tsc_hz()*15) {
        uint64_t chunk=count-loaded; if (chunk>65536) chunk=65536;
        if (vfs_read(&f,data+loaded,chunk)!=chunk) break;
        loaded+=chunk;
        wm_resource_checkpoint();
    }
    vfs_close(&f);
    if (!data || loaded!=count) { kfree(data); return nullptr; }
    *size=count; return data;
}
// Ranged read for .wiz metadata: fetches only `len` bytes at `offset`, so the
// desktop can read a name and icon from a package without loading all of it.
// len == 0 just reports the file size.
static bool wm_resource_range(const char* path, uint64_t offset, void* buffer, uint64_t len, uint64_t* total) {
    wm_resource_checkpoint();
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
            wm_resource_checkpoint();
        }
    }
    vfs_close(&f); return ok;
}
static int wm_resource_list(const char* path, uint64_t index, char* name, bool* directory) {
    wm_resource_checkpoint();
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
static bool g_hud_sample_due=true;
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

static uint32_t wm_hid_modifiers(uint8_t bits) {
    return ((bits&0x11)?WIN_MOD_CTRL:0)|((bits&0x22)?WIN_MOD_SHIFT:0)|
           ((bits&0x44)?WIN_MOD_ALT:0)|((bits&0x88)?WIN_MOD_GUI:0);
}
#define WIN_KEY_HOME   0x1100u
#define WIN_KEY_END    0x1101u
#define WIN_KEY_DELETE 0x1102u
#define WIN_ROLE_APP 0u
#define WIN_ROLE_FILE_DIALOG 1u

#define WIN_EVENT_NONE         0u
#define WIN_EVENT_MOUSE_DOWN   1u
#define WIN_EVENT_MOUSE_UP     2u
#define WIN_EVENT_MOUSE_MOVE   3u
#define WIN_EVENT_MOUSE_SCROLL 4u
#define WIN_EVENT_KEY_DOWN     5u
#define WIN_EVENT_KEY_UP       6u
#define WIN_EVENT_CLOSE        7u
#define WIN_EVENT_RESIZE       8u

struct WinEvent {
    uint32_t type;    // WIN_EVENT_*
    int32_t  x;       // client-relative x
    int32_t  y;       // client-relative y
    uint32_t key;     // ASCII or scancode / button (1=L, 2=R, 4=M)
    uint32_t mods;    // WIN_MOD_*
    int32_t  delta;   // scroll dy (+1 or -1)
};

#define WIN_EVENT_QUEUE_SIZE 64

struct PublishedPixels {
    void* allocation;
    uint32_t* pixels;
    uint64_t capacity, retired_frame;
    int width, height;
    uint64_t revision;
};
static uint64_t g_surface_publication=0;
static uint64_t g_compositor_frame = 1;
struct Window {
    int id;
    char title[64];
    char executable[256];
    int32_t x, y, w, h;
    uint32_t* buffer;
    uint32_t buffer_pitch;
    PublishedPixels published[2];
    unsigned published_slot;
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
    bool restore_drag_pending;
    int32_t ghost_x, ghost_y;
    int32_t drag_off_x, drag_off_y;



    // Per-window event queue
    WinEvent event_queue[WIN_EVENT_QUEUE_SIZE];
    uint32_t event_head;
    uint32_t event_tail;
    int32_t canvas_width,canvas_height,min_w,min_h;
    bool native_resize,resize_pending;
    bool premultiplied; // owner opt-in; legacy client alpha is ignored
    bool shadow_enabled;
    unsigned resize_edges;
    int32_t ghost_w,ghost_h,resize_mouse_x,resize_mouse_y;
    ResizeRect resize_original;
    Window* retired_next;
    bool minimized,maximized;
    ResizeRect restore_rect;
    uint32_t role; // Semantic window role; ownership/executable remain those of the caller.
    struct TitleBarCache* titlebar_cache;
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
    TtyIrqGuard guard;
    uint32_t next_head = (win->event_head + 1) % WIN_EVENT_QUEUE_SIZE;
    if (next_head == win->event_tail) {
        win->event_tail = (win->event_tail + 1) % WIN_EVENT_QUEUE_SIZE;
    }
    win->event_queue[win->event_head] = ev;
    win->event_head = next_head;
    task_wake_console(win->owner_pid);
    return true;
}

static inline bool wm_win_pop_event(Window* win, WinEvent* out_ev) {
    if (!win || !out_ev) return false;
    TtyIrqGuard guard;
    if (win->close_pending) {
        win->close_pending = false;
        *out_ev = {};
        out_ev->type = WIN_EVENT_CLOSE;
        return true;
    }
    if(win->resize_pending){win->resize_pending=false;*out_ev={};out_ev->type=WIN_EVENT_RESIZE;
        out_ev->x=win->w;out_ev->y=win->h;return true;}
    if (win->event_head == win->event_tail) return false;
    *out_ev = win->event_queue[win->event_tail];
    win->event_tail = (win->event_tail + 1) % WIN_EVENT_QUEUE_SIZE;
    return true;
}

// Pointer events use the old canvas coordinates. Keep keyboard input when
// changing the canvas, but discard pointer events that no longer match it.
static inline void wm_discard_pointer_events(Window* win) {
    TtyIrqGuard guard;
    uint32_t write=win->event_tail;
    for(uint32_t read=win->event_tail;read!=win->event_head;read=(read+1)%WIN_EVENT_QUEUE_SIZE){
        const WinEvent ev=win->event_queue[read];
        if(ev.type==WIN_EVENT_KEY_DOWN||ev.type==WIN_EVENT_KEY_UP){
            win->event_queue[write]=ev;write=(write+1)%WIN_EVENT_QUEUE_SIZE;
        }
    }
    win->event_head=write;
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
    if(win->canvas_width&&win->canvas_height&&(win->canvas_width!=win->w||win->canvas_height!=win->h)){
        x1=(int64_t)x1*win->w/win->canvas_width;y1=(int64_t)y1*win->h/win->canvas_height;
        x2=((int64_t)x2*win->w+win->canvas_width-1)/win->canvas_width;
        y2=((int64_t)y2*win->h+win->canvas_height-1)/win->canvas_height;
    }
    int32_t sx1, sy1, sx2, sy2;
    if (!surface_damage_to_screen(win->x, win->y, win->w, win->h, x1, y1, x2, y2,
                                  (int32_t)fb->width, (int32_t)fb->height, &sx1, &sy1, &sx2, &sy2)) return;
    uint32_t ux1 = (uint32_t)sx1, ux2 = (uint32_t)sx2 - 1;
    uint32_t uy1 = (uint32_t)sy1, uy2 = (uint32_t)sy2 - 1;
    wm_dirty_add(ux1, ux2, uy1, uy2);
    g_needs_refresh = true;
}
static inline void wm_damage_window_full(Window* win) {
    if (!win || !win->visible || !fb) return;
    int32_t x1 = win->x;
    int32_t y1 = win->y - (int32_t)g_title_h;
    int32_t x2 = win->x + win->w;
    int32_t y2 = win->y + win->h;
    if(win->shadow_enabled&&!win->maximized){
        auto r=window_shadow_bounds({x1,y1,x2,y2});
        x1=r.x1;y1=r.y1;x2=r.x2;y2=r.y2;
    }
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > (int32_t)fb->width) x2 = (int32_t)fb->width;
    if (y2 > (int32_t)fb->height) y2 = (int32_t)fb->height;
    if (x2 > x1 && y2 > y1) {
        wm_dirty_add((uint32_t)x1, (uint32_t)(x2 - 1), (uint32_t)y1, (uint32_t)(y2 - 1));
        g_needs_refresh = true;
    }
}

static void wm_damage_window_title(const Window* win) {
    if(!fb||!win||!win->visible||win->minimized)return;
    const auto title=visibility_clip(win->x,win->y,win->w,0,g_title_h,fb->width,fb->height);
    if(visibility_valid(title))wm_dirty_add(title.x1,title.x2-1,title.y1,title.y2-1);
}

// Call with interrupts disabled while examining the live window stack.
static bool wm_desktop_stats_visible() {
    for(int i=0;i<g_window_count;++i){
        const auto* win=g_window_list[i];
        if(win&&win->visible&&!win->minimized&&win->maximized)return false;
    }
    return true;
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
struct MenuPixelCache { uint8_t* pixels=nullptr;uint64_t capacity=0,key=0; };
static MenuPixelCache g_menu_pixel_cache{};
static void wm_prepare_menu_cache(){
    uint64_t bytes=(uint64_t)fb->pitch*fb->height;
    if(bytes>64*1024*1024 || g_menu_pixel_cache.capacity>=bytes)return;
    auto* pixels=(uint8_t*)kmalloc(bytes);
    if(!pixels)return;
    kfree(g_menu_pixel_cache.pixels);
    g_menu_pixel_cache={pixels,bytes,0};
}
static void wm_taskbar_close();
static bool wm_taskbar_popup_open();
static bool wm_taskbar_escape(char c,uint32_t mods);
// Action IDs never overlap catalog indices.
static constexpr int MENU_RESTART = -2, MENU_SHUTDOWN = -3;
static inline int32_t wm_menu_scale(int32_t value, int32_t minimum) {
    int32_t scaled = (int32_t)(fb->height * value / 1080);
    return scaled < minimum ? minimum : scaled;
}
static inline int32_t wm_menu_header() { return wm_menu_scale(56, 44); }
static inline int32_t wm_menu_footer() { return wm_menu_scale(64, 52); }
static inline int32_t wm_menu_row() { return wm_menu_scale(38, 26); }
static inline int32_t wm_menu_pad() { return wm_menu_scale(14, 10); }

// Panel geometry (scaled like the rest of the UI). The app list starts below the header.
static inline void wm_menu_geom(int32_t* px, int32_t* py, int32_t* pw, int32_t* ph) {
    uint32_t sn = fb->height, sd = 1080;
    int32_t w = wm_menu_scale(380, 290);
    if (w > (int32_t)fb->width - 12) w = (int32_t)fb->width - 12;
    int32_t hdr_h = wm_menu_header(), row_h = wm_menu_row(), pad = wm_menu_pad();

    int rows = g_menu_app_count > 0 ? g_menu_app_count : 1;
    int32_t h = hdr_h + rows * row_h + pad * 2 + wm_menu_footer();
    int32_t max_h = (int32_t)((sn * 740) / sd);
    int32_t available = (int32_t)fb->height - (int32_t)g_taskbar_h - 12;
    if (max_h > available) max_h = available;
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

    int32_t hdr_h = wm_menu_header(), row_h = wm_menu_row(), pad = wm_menu_pad();
    int32_t footer_top = py + ph - wm_menu_footer();
    if (my >= footer_top + pad && my < py + ph - pad) {
        int32_t button_w = (pw - pad * 3) / 2;
        if (mx >= px + pad && mx < px + pad + button_w) return MENU_RESTART;
        if (mx >= px + pad * 2 + button_w && mx < px + pw - pad) return MENU_SHUTDOWN;
    }
    if (mx < px + pad || mx >= px + pw - pad) return -1;

    int32_t list_top = py + hdr_h + pad;
    int32_t visible_rows = (footer_top - pad - list_top) / row_h;
    if (my < list_top || my >= list_top + visible_rows * row_h) return -1;
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
    gpu_menu_begin();
    wm_taskbar_close();
    g_menu_open = true;
    // Rapid reopenings reuse the complete snapshot. Refresh at the next open
    // after five seconds, preserving discovery of externally replaced assets.
    uint64_t now=rdtsc_val();
    const bool refresh=!g_resource_refresh_ticks||!tsc_hz||now-g_resource_refresh_ticks>=tsc_hz*5;
#if GPU_FRAME_DIAGNOSTICS
    GpuGapScope opening(GpuGapActivity::StartOpen,refresh?"refresh":"cached");
#endif
    if(auto* trace=gpu_menu_open_trace()){
        trace->requested=now;
        trace->refresh=refresh;
    }
    if (refresh)
        wm_menu_refresh();
    // Menu icons are already part of the resident catalog demand.
    wm_menu_dirty_rect();
}

static inline void wm_menu_close() {
    if (!g_menu_open) return;
#if GPU_FRAME_DIAGNOSTICS
    GpuGapScope closing(GpuGapActivity::StartClose);
#endif
    g_menu_open = false;
    wm_menu_dirty_rect(); // geometry unchanged: the list stays cached until next open
}

static inline void wm_menu_hover_dirty(int previous,int current){
    if(previous==current||!g_menu_open)return;
    int x,y,w,h;wm_menu_geom(&x,&y,&w,&h);
    int pad=wm_menu_pad();
    auto damage_item=[&](int item){
        if(item==-1)return;
        int left=x+pad,right=x+w-pad,top,bottom;
        if(item>=0){
            top=y+wm_menu_header()+pad+item*wm_menu_row();
            bottom=top+wm_menu_row()-3;
        }else{
            int width=(w-3*pad)/2;
            if(item==MENU_SHUTDOWN)left+=width+pad;
            right=left+width;
            top=y+h-wm_menu_footer()+pad;bottom=y+h-pad;
        }
        if(right>left&&bottom>top)wm_dirty_add(left,right-1,top,bottom-1);
    };
    damage_item(previous);damage_item(current);g_needs_refresh=true;
}

static TextClipboard g_text_clipboard;
static bool g_clipboard_open=false;
static int g_clipboard_selected=0,g_clipboard_first=0,g_clipboard_target=-1;
static constexpr int CLIPBOARD_HEADER=40,CLIPBOARD_ROW=68,CLIPBOARD_FOOTER=32,
    CLIPBOARD_PAD=10,CLIPBOARD_GAP=6,CLIPBOARD_VISIBLE_ROWS=4,
    CLIPBOARD_CLEAR_WIDTH=116,CLIPBOARD_ACTION_WIDTH=32,CLIPBOARD_ACTION_HEIGHT=23,
    CLIPBOARD_PIN_TOP=6,CLIPBOARD_DELETE_TOP=33;
struct ClipboardPanel {
    int x,y,w,h,rows;
    bool scrollable;
    int list_top()const{return y+CLIPBOARD_HEADER+CLIPBOARD_PAD;}
    int card_right()const{return x+w-CLIPBOARD_PAD-(scrollable?8:0);}
    int action_left()const{return card_right()-8-CLIPBOARD_ACTION_WIDTH;}
    int clear_left()const{return x+w-6-CLIPBOARD_CLEAR_WIDTH;}
};
static ClipboardPanel wm_clipboard_panel(){
    int w=(int)fb->width-16;if(w>440)w=440;
    int available=(int)fb->height-(int)g_taskbar_h-16-CLIPBOARD_HEADER-CLIPBOARD_FOOTER-2*CLIPBOARD_PAD;
    int rows=available/CLIPBOARD_ROW;
    int count=g_text_clipboard.history_size();
    if(rows>count)rows=count;
    if(rows<1)rows=1;
    if(rows>CLIPBOARD_VISIBLE_ROWS)rows=CLIPBOARD_VISIBLE_ROWS;
    int h=CLIPBOARD_HEADER+2*CLIPBOARD_PAD+rows*CLIPBOARD_ROW+CLIPBOARD_FOOTER;
    return {(int)fb->width-w-8,(int)fb->height-(int)g_taskbar_h-h-8,w,h,rows,count>rows};
}
static void wm_clipboard_damage(){
    if(!fb||!g_clipboard_open)return;
    auto p=wm_clipboard_panel();
    wm_dirty_add(p.x,p.x+p.w-1,p.y,p.y+p.h-1);
    g_needs_refresh=true;
}
static void wm_clipboard_close(){
    wm_clipboard_damage();
    g_clipboard_open=false;
}
static void wm_gui_key_update(WinKeyTap& tap,uint32_t mods,bool other_key_held){
    if(!tap.update(mods,other_key_held)||!fb)return;
    TtyIrqGuard guard;
    if(g_menu_open)wm_menu_close();
    else {wm_clipboard_close();wm_menu_open();}
}
static void wm_clipboard_paste(int target);
static void wm_clipboard_accept(unsigned index){
    if(!g_text_clipboard.select(index))return;
    wm_clipboard_close();
    wm_clipboard_paste(g_clipboard_target);
}
enum class ClipboardAction { Paste, Pin, Delete };
struct ClipboardHit {
    int index=-1;
    ClipboardAction action=ClipboardAction::Paste;
    bool operator==(const ClipboardHit&)const=default;
};
static ClipboardHit wm_clipboard_hit(int x,int y){
    if(!g_clipboard_open)return {};
    auto p=wm_clipboard_panel();
    int offset=y-p.list_top();
    if(x<p.x+CLIPBOARD_PAD||x>=p.card_right()||offset<0||offset>=p.rows*CLIPBOARD_ROW||
        offset%CLIPBOARD_ROW>=CLIPBOARD_ROW-CLIPBOARD_GAP)return {};
    int index=g_clipboard_first+offset/CLIPBOARD_ROW;
    if(index>=(int)g_text_clipboard.history_size())return {};
    if(x>=p.action_left()&&x<p.action_left()+CLIPBOARD_ACTION_WIDTH){
        int y=offset%CLIPBOARD_ROW;
        if(y>=CLIPBOARD_PIN_TOP&&y<CLIPBOARD_PIN_TOP+CLIPBOARD_ACTION_HEIGHT)return {index,ClipboardAction::Pin};
        if(y>=CLIPBOARD_DELETE_TOP&&y<CLIPBOARD_DELETE_TOP+CLIPBOARD_ACTION_HEIGHT)return {index,ClipboardAction::Delete};
    }
    return {index,ClipboardAction::Paste};
}
static void wm_clipboard_move(int delta){
    TtyIrqGuard guard;
    int last=(int)g_text_clipboard.history_size()-1;
    if(last<0){g_clipboard_first=g_clipboard_selected=0;wm_clipboard_damage();return;}
    auto p=wm_clipboard_panel();
    g_clipboard_selected+=delta;
    if(g_clipboard_selected<0)g_clipboard_selected=0;
    if(g_clipboard_selected>last)g_clipboard_selected=last;
    int last_page=last-p.rows+1;
    if(last_page<0)last_page=0;
    if(g_clipboard_first>last_page)g_clipboard_first=last_page;
    if(g_clipboard_selected<g_clipboard_first)g_clipboard_first=g_clipboard_selected;
    if(g_clipboard_selected>=g_clipboard_first+p.rows)g_clipboard_first=g_clipboard_selected-p.rows+1;
    wm_clipboard_damage();
}
static void wm_clipboard_scroll(int delta){
    TtyIrqGuard guard;
    auto p=wm_clipboard_panel();
    if(!p.scrollable)return;
    int last=(int)g_text_clipboard.history_size()-p.rows;
    int first=g_clipboard_first+delta;
    if(first<0)first=0;
    if(first>last)first=last;
    if(first==g_clipboard_first)return;
    g_clipboard_first=first;
    if(g_clipboard_selected<first)g_clipboard_selected=first;
    if(g_clipboard_selected>=first+p.rows)g_clipboard_selected=first+p.rows-1;
    wm_clipboard_damage();
}
static void wm_clipboard_toggle_pin(unsigned index){
    if(g_text_clipboard.toggle_pin(index)){g_clipboard_selected=index;wm_clipboard_damage();}
}
static void wm_clipboard_delete(unsigned index){
    if(index>=g_text_clipboard.history_size())return;
    wm_clipboard_damage();
    g_text_clipboard.erase(index);
    g_clipboard_selected=index;
    wm_clipboard_move(0);
}
static bool wm_clipboard_click(int x,int y){
    if(!g_clipboard_open)return false;
    TtyIrqGuard guard;
    auto p=wm_clipboard_panel();
    if(x>=p.x&&x<p.x+p.w&&y>=p.y&&y<p.y+p.h){
        if(y>=p.y+6&&y<p.y+CLIPBOARD_HEADER-6&&x>=p.clear_left()&&x<p.x+p.w-6){
            wm_clipboard_damage();
            g_text_clipboard.clear_unpinned();g_clipboard_selected=g_clipboard_first=0;wm_clipboard_damage();
        }else{
            auto hit=wm_clipboard_hit(x,y);
            if(hit.index>=0){
                if(hit.action==ClipboardAction::Pin)wm_clipboard_toggle_pin(hit.index);
                else if(hit.action==ClipboardAction::Delete)wm_clipboard_delete(hit.index);
                else wm_clipboard_accept(hit.index);
            }
        }
    }else wm_clipboard_close();
    return true;
}
static bool wm_clipboard_key(char c,uint32_t key,uint32_t mods,bool repeat){
    if(!g_clipboard_open&&!((c=='v'||c=='V')&&mods==WIN_MOD_GUI))return false;
    TtyIrqGuard guard;
    if((c=='v'||c=='V')&&mods==WIN_MOD_GUI){
        if(!repeat&&fb&&fb->width>=236&&
            fb->height>=g_taskbar_h+16+CLIPBOARD_HEADER+2*CLIPBOARD_PAD+CLIPBOARD_ROW+CLIPBOARD_FOOTER){
            if(g_clipboard_open)wm_clipboard_close();
            else {
                wm_menu_close();wm_taskbar_close();g_clipboard_selected=g_clipboard_first=0;
                g_clipboard_target=g_focused_win_id;g_clipboard_open=true;wm_clipboard_damage();
            }
        }
        return true;
    }
    if(key==0x52)wm_clipboard_move(-1);
    else if(key==0x51)wm_clipboard_move(1);
    else if(c==27)wm_clipboard_close();
    else if(!repeat&&!(mods&~WIN_MOD_SHIFT)&&(c=='p'||c=='P'))wm_clipboard_toggle_pin(g_clipboard_selected);
    else if(!repeat&&!mods&&key==0x4c)wm_clipboard_delete(g_clipboard_selected);
    else if(!repeat&&(c=='\n'||c=='\r'))wm_clipboard_accept(g_clipboard_selected);
    return true;
}

static uint32_t wm_icon_size(uint32_t value, uint32_t minimum, uint32_t maximum) {
    return value<minimum ? minimum : value>maximum ? maximum : value;
}
static resources::Store g_resource_store{{wm_resource_read,wm_resource_list,wm_resource_range,wm_resource_checkpoint}};
static uint64_t wm_trace_cpu_cycles(){
    TtyIrqGuard guard;
    scheduler_account_cpu(rdtsc_val());
    return g_current_task?g_current_task->cpu_cycles:0;
}
static void wm_damage_catalog_ui();
static void wm_resources_reload() {
    auto* menu_trace=gpu_menu_open_trace();
    uint64_t resource_started=rdtsc_val();
    uint64_t trace_cpu=GPU_RCS_ALPHA?wm_trace_cpu_cycles():0;
    if(menu_trace){
        if(!menu_trace->resource_start)menu_trace->resource_start=resource_started;
    }
    uint32_t row=wm_icon_size(fb->height*38/1080,26,256);
    resources::Demand demand{};
    uint64_t flags,generation;
    bool refresh;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    generation=g_resource_demand_generation;
    refresh=g_resources_refresh; g_resources_refresh=false;
    {
        int32_t header=wm_menu_header(), pad=wm_menu_pad();
        int32_t room=(int32_t)(fb->height*740/1080)-header-pad*2-wm_menu_footer();
        if (room>0) demand.menu_rows=(room+row-1)/row;
    }
    for (int i=0;i<g_window_count;++i) {
        const Window* win=g_window_list[i];
        if (!win || !win->visible) continue;
        if (!win->is_system && win->role==WIN_ROLE_FILE_DIALOG) demand.system[resources::file_dialog_icon]=true;
        else if (win->is_system && win->type>=0 && win->type<4) demand.system[win->type]=true;
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
    resources::Catalog* next;
    {
#if GPU_FRAME_DIAGNOSTICS
        GpuGapScope loading(GpuGapActivity::Resources,refresh?"rescan":"window icons");
#endif
        next=resources::build(g_resource_store,wm_icon_size(row-10,14,128),
                                wm_icon_size(g_title_h-8,12,64),wm_icon_size(g_taskbar_h-16,16,64),
                                demand,refresh ? nullptr : g_resources_catalog);
    }
    // An open request may arrive while the worker is already loading.
    if(!menu_trace)menu_trace=gpu_menu_open_trace();
    if(menu_trace){
        if(!menu_trace->resource_start)menu_trace->resource_start=resource_started;
        menu_trace->resource_done=rdtsc_val();
        menu_trace->resource_cpu+=wm_trace_cpu_cycles()-trace_cpu;
    }
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
            {
#if GPU_FRAME_DIAGNOSTICS
                GpuGapScope publishing(GpuGapActivity::CatalogPublish);
#endif
                if (g_menu_open) wm_menu_dirty_rect();
                previous=g_resources_catalog; g_resources_catalog=next;++g_decoration_epoch;
                if(menu_trace)menu_trace->resource_commit=rdtsc_val();
                if (refresh) g_resource_refresh_ticks=rdtsc_val();
                wm_damage_catalog_ui();
            }
            if (flags&(1u<<9)) __asm__ volatile("sti" ::: "memory");
            break;
        }
        if (flags&(1u<<9)) __asm__ volatile("sti" ::: "memory");
        task_sleep_ms(1);
    }
    if (previous!=&g_empty_catalog) resources::release(previous);
}
static void wm_resource_worker() {
    // Let the timer preempt catalog loading, including its disk reads.
    __asm__ volatile("sti" ::: "memory");
    for (;;) {
        if (g_resources_requested) {
            g_resources_requested=false;
            g_resource_cooperative=true;g_resource_slice_started=rdtsc_val();
            wm_resources_reload();
            g_resource_cooperative=false;
        }
        task_sleep_ms(50);
    }
}
static const resources::Icons& wm_window_icons(const Window* window) {
    if(!window->is_system&&window->role==WIN_ROLE_FILE_DIALOG)return g_resources_catalog->system[resources::file_dialog_icon];
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
    ui_active_fonts = atlas->fonts; ui_active_font_count = 8;++g_decoration_epoch;
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
    r->visible = win->visible && !win->minimized;
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
struct ClipboardPreview {
    char lines[2][96];
    bool pinned;
};
static void wm_clipboard_preview(const TextClipboard::Entry& entry,ClipboardPreview& preview,
                                 const UiFont* font,int width){
    preview.pinned=entry.pinned;
    unsigned offset=0;
    for(int row=0;row<2;++row){
        auto& line=preview.lines[row];
        unsigned n=0,wrap_length=0,wrap_offset=0;
        int used=0;
        while(offset<entry.length&&n<sizeof(line)-4){
            unsigned char c=entry.bytes[offset];
            if(c=='\r'){++offset;continue;}
            if(c=='\n'){++offset;break;}
            if(c<32||c==127)c=' ';
            int advance=ui_font_glyph(font,c)->advance;
            if(used+advance>width&&n){
                if(!row&&wrap_length){offset=wrap_offset;n=wrap_length;}
                break;
            }
            if(c==' '&&n){wrap_offset=offset+1;wrap_length=n;}
            line[n++]=c;++offset;used+=advance;
        }
        if(row==1&&offset<entry.length){
            int dots=3*ui_font_glyph(font,'.')->advance;
            while(n&&(line[n-1]==' '||used+dots>width))used-=ui_font_glyph(font,line[--n])->advance;
            line[n++]='.';line[n++]='.';line[n++]='.';
        }
        line[n]=0;
    }
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
static unsigned g_desktop_fps=0;
static inline int32_t wm_clock_width() {
    uint32_t scale = wm_clock_scale();
    int32_t time_width = wm_text_width(CLOCK_SHOW_SECONDS ? "88:88:88 PM" : "88:88 PM", scale);
    int32_t date_width = wm_text_width("8888-88-88", scale);
    return (time_width > date_width ? time_width : date_width) + 20;
}
static inline int32_t wm_fps_width() {
    return wm_text_width("888 FPS",wm_clock_scale())+16;
}
static inline int32_t wm_tray_w() {
    return wm_clock_width()+wm_fps_width()+g_taskbar_h;
}
static int wm_volume_tray_x(){return (int)fb->width-6-wm_clock_width()-g_taskbar_h;}
static void wm_draw_volume_icon(int x,int y,int size,unsigned percent,bool muted,int min_y,int max_y){
    for(int py=0;py<size;++py){
        int yy=y+py;if(yy<min_y||yy>max_y||yy<g_ui_clip_top||yy>=g_ui_clip_bottom||yy<0||yy>=(int)fb->height)continue;
        for(int px=0;px<size;++px){
            int xx=x+px;if(xx<g_ui_clip_left||xx>=g_ui_clip_right||xx<0||xx>=(int)fb->width)continue;
            unsigned alpha=volume_icon_coverage(percent,muted,px,py,size);if(!alpha)continue;
            uint32_t* pixel=(uint32_t*)(g_master_backbuffer+(uint64_t)yy*fb->pitch)+xx;
            uint32_t color=0xff000000;
            for(unsigned shift=0;shift<24;shift+=8)color|=(((((*pixel>>shift)&255)*(16-alpha)+0xee*alpha+8)/16)<<shift);
            *pixel=color;
        }
    }
}
static int32_t wm_tray_damage_left(){
    int32_t x=(int32_t)fb->width-wm_tray_w()-14;
    return x>0?x:0;
}

// Mark the tray (plus the 2px separator left of it) dirty so the compositor repaints the clock.
static inline void wm_clock_damage() {
    if (!fb) return;
    int32_t x1 = wm_tray_damage_left();
    int32_t y1 = (int32_t)fb->height - (int32_t)g_taskbar_h;
    if (y1 < 0) y1 = 0;
    wm_dirty_add((uint32_t)x1, fb->width - 1, (uint32_t)y1, fb->height - 1);
    g_needs_refresh = true;
}

static uint64_t g_titlebar_cache_renders=0,g_titlebar_cache_hits=0,g_titlebar_cache_bytes=0;
static int g_titlebar_hover_win=0,g_titlebar_hover_control=WINDOW_NO_CONTROL;
static void wm_draw_window_chrome(Window* win,int x,int y,int width,int min_y,int max_y,const Image& icon) {
    if(!fb||!g_master_backbuffer)return;
    VisibleRect clip{g_ui_clip_left,min_y,g_ui_clip_right,max_y+1};
    if(!visibility_valid(visibility_intersect({x,y,x+width,y+(int)g_title_h},clip)))return;
    TitleBarKey key{};key.width=width;key.height=g_title_h;key.font=wm_ui_font(g_font_scale_100);
    key.icon=icon;key.resource_epoch=g_decoration_epoch;key.focused=win->id==g_focused_win_id;
    key.closable=win->closable;key.maximized=win->maximized;
    key.hover=win->id==g_titlebar_hover_win?g_titlebar_hover_control:WINDOW_NO_CONTROL;
    for(unsigned i=0;i<sizeof(key.title);++i){key.title[i]=win->title[i];if(!win->title[i])break;}
    TitleBarTarget target{(uint32_t*)g_master_backbuffer,(int)fb->width,(int)fb->height,(int)fb->pitch/4};
    uint64_t bytes=(uint64_t)width*g_title_h*4;
    if(width<=0||g_title_h>256||bytes>2*1024*1024){title_bar_render(key,target,x,y,clip);return;}
    auto* cache=win->titlebar_cache;
    if(!cache){
        cache=(TitleBarCache*)kmalloc(sizeof(TitleBarCache));
        if(cache){memset(cache,0,sizeof(*cache));win->titlebar_cache=cache;}
    }
    if(!cache){title_bar_render(key,target,x,y,clip);return;}
    if(cache->capacity<bytes){
        auto* pixels=(uint32_t*)kmalloc(bytes);
        if(!pixels){title_bar_render(key,target,x,y,clip);return;}
        g_titlebar_cache_bytes-=cache->capacity;kfree(cache->pixels);
        cache->pixels=pixels;cache->capacity=bytes;cache->valid=false;g_titlebar_cache_bytes+=bytes;
    }
    if(!cache->valid||!title_bar_equal(cache->key,key)){
        title_bar_render(key,{cache->pixels,width,(int)g_title_h,width},0,0,{0,0,width,(int)g_title_h});
        cache->key=key;cache->valid=true;++g_titlebar_cache_renders;
    }else ++g_titlebar_cache_hits;
    title_bar_blit(cache->pixels,width,g_title_h,target,x,y,clip);
}

// =============================================================================
//  OVERLAY LAYER — cursor + ghost drag outlines
//
//  These are painted DIRECTLY onto VRAM. master_backbuffer is ALWAYS
//  cursor-free. This decouples cursor speed from compositor cost entirely,
//  the same way a hardware sprite overlay works.
// =============================================================================

// Currently live on VRAM beyond master_backbuffer:
static OverlayFrame g_presented_overlay{{-999,-999,0,0},{},0};
static bool g_gpu_present_pending=false;
static uint32_t wm_cursor_color(unsigned edges,int x,int y,uint8_t buttons);
static const uint32_t* wm_cursor_sprite(const OverlayCursor& cursor);
static void wm_capture_overlay(OverlayFrame* frame,const OverlayCursor& cursor) {
    static_assert(OverlayFrame::capacity>=MAX_WINDOWS);
    frame->cursor=cursor;frame->count=0;
    for(int i=0;i<g_window_count;++i){
        Window* win=g_window_list[i];
        if(!win||!win->visible||win->minimized||(!win->ghost_dragging&&!win->resize_edges))continue;
        auto& r=frame->outlines[frame->count++];
        wm_get_win_rect_from_pos(win,win->ghost_x,win->ghost_y,&r.x,&r.y,&r.w,&r.h);
        if(win->resize_edges){r.w=win->ghost_w;r.h=win->ghost_h+(int)g_title_h;}
    }
}
static inline void vram_present_span(int left,int right,int y,const OverlayFrame& frame) {
    OverlayTarget target{(uint8_t*)fb->address,g_master_backbuffer,(int)fb->width,(int)fb->height,(int)fb->pitch};
    overlay_render_span(target,left,right,y,frame,wm_cursor_sprite(frame.cursor));
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

// Both compositor presentation and the direct VRAM overlay use this sprite.
// Resize cursors are centered on the actual edge/corner, unlike the arrow's tip.
static unsigned wm_cursor_edges(int mx,int my) {
    for(int i=0;i<g_window_count;++i)if(g_window_list[i]->resize_edges)return g_window_list[i]->resize_edges;
    for(int s=g_window_count-1;s>=0;--s){Window* win=g_window_list[g_window_order[s]];
        if(!win||!win->visible||win->minimized)continue;
        if(mx>=win->x&&mx<win->x+win->w&&my>=win->y-(int)g_title_h&&my<win->y+win->h){
            if(win->maximized||window_control_hit(win->x,win->y-g_title_h,win->w,g_title_h,mx,my)>=0)return 0;
            return resize_hit({win->x,win->y,win->w,win->h},g_title_h,mx,my);
        }
    }
    return 0;
}
static uint32_t wm_cursor_color(unsigned edges,int x,int y,uint8_t buttons) {
    if(edges){unsigned pixel=resize_cursor_pixel(edges,x,y);
        return pixel==2?0xFFF8F3FF:pixel==1?0xFF17121E:0;}
    if(x<0||x>=CURSOR_W||y<0||y>=CURSOR_H)return 0;
    unsigned pixel=g_cursor_bitmap[y][x];
    return pixel==1?0xFFFFFFFF:pixel==2?((buttons&1)?0xFFFF0000:(buttons&2)?0xFF00FFFF:0xFF000000):0;
}
static const uint32_t* wm_cursor_sprite(const OverlayCursor& cursor) {
    static uint32_t pixels[CURSOR_SPRITE_SIZE*CURSOR_SPRITE_SIZE];
    static bool ready=false;static unsigned edges=0;static uint8_t buttons=0;
    if(!ready||edges!=cursor.edges||buttons!=cursor.buttons){
        for(int y=0;y<CURSOR_SPRITE_SIZE;++y)for(int x=0;x<CURSOR_SPRITE_SIZE;++x)
            pixels[y*CURSOR_SPRITE_SIZE+x]=wm_cursor_color(cursor.edges,x-CURSOR_SPRITE_ORIGIN,y-CURSOR_SPRITE_ORIGIN,cursor.buttons);
        edges=cursor.edges;buttons=cursor.buttons;ready=true;
    }
    return pixels;
}
// ─── Ghost frame geometry ────────────────────────────────────────────────────

static inline int wm_find_top_window(int32_t mx, int32_t my) {
    for (int s = g_window_count - 1; s >= 0; s--) {
        int idx = g_window_order[s];
        Window* win = g_window_list[idx];
        if (!win || !win->visible || win->minimized) continue;
        WinRect r; wm_get_win_rect(win, &r);
        if (mx >= r.x1 && mx < r.x2 && my >= r.y1 && my < r.y2) return idx;
    }
    return -1;
}

// Hover changes require scene damage even when only the pointer moved. Cache
// keys ignore absolute position, so moving a window reuses its title bitmap.
static void wm_titlebar_hover_damage() {
    static WindowControlRect previous{};
    TtyIrqGuard guard;
    int id=0,control=WINDOW_NO_CONTROL;WindowControlRect current{};
    int index=wm_find_top_window(g_mouse_x,g_mouse_y);
    if(index>=0){
        auto* win=g_window_list[index];
        control=window_control_hit(win->x,win->y-g_title_h,win->w,g_title_h,g_mouse_x,g_mouse_y);
        if(control==WINDOW_CLOSE&&!win->closable)control=WINDOW_NO_CONTROL;
        if(control!=WINDOW_NO_CONTROL){id=win->id;current=window_control_rect(win->x,win->y-g_title_h,win->w,g_title_h,control);}
    }
    if(id==g_titlebar_hover_win&&control==g_titlebar_hover_control&&
       current.x==previous.x&&current.y==previous.y&&current.w==previous.w&&current.h==previous.h)return;
    auto damage=[](WindowControlRect r){
        if(!fb||r.w<=0||r.h<=0)return;
        auto clipped=visibility_intersect({r.x,r.y,r.x+r.w,r.y+r.h},{0,0,(int)fb->width,(int)fb->height});
        if(visibility_valid(clipped))wm_dirty_add(clipped.x1,clipped.x2-1,clipped.y1,clipped.y2-1);
    };
    damage(previous);damage(current);previous=current;
    g_titlebar_hover_win=id;g_titlebar_hover_control=control;g_needs_refresh=true;
}

static inline void wm_raise_window(int idx) {
    TtyIrqGuard guard;
    if (idx < 0 || idx >= g_window_count) return;
    Window* win = g_window_list[idx];
    if (win && !win->minimized && g_focused_win_id == win->id &&
        g_window_order[g_window_count - 1] == idx) return;
    const int previous_focus=g_focused_win_id;
    const bool stats_visible=wm_desktop_stats_visible();
    if (win) { win->minimized=false;g_focused_win_id = win->id; }
    int old_pos = -1;
    for (int i = 0; i < g_window_count; i++) if (g_window_order[i] == idx) old_pos = i;
    if (old_pos != -1 && old_pos < g_window_count - 1) {
        for (int i = old_pos; i < g_window_count - 1; i++) g_window_order[i] = g_window_order[i+1];
        g_window_order[g_window_count - 1] = idx;
    }
    if(fb){
        if(wm_taskbar_popup_open()){
            wm_dirty_add(0,fb->width-1,0,fb->height-1);
        }else{
            wm_damage_window_full(win);
            for(int i=0;i<g_window_count;++i){
                const auto* previous=g_window_list[i];
                if(previous&&previous!=win&&previous->id==previous_focus){
                    wm_damage_window_title(previous);
                    break;
                }
            }
            if(stats_visible!=wm_desktop_stats_visible())
                wm_dirty_add(0,fb->width-1,0,59);
            wm_dirty_add(0,fb->width-1,fb->height-g_taskbar_h,fb->height-1);
        }
    }
    g_needs_refresh = true;
}

static inline bool wm_is_in_title(int idx, int32_t mx, int32_t my) {
    if (idx < 0 || idx >= g_window_count) return false;
    Window* win = g_window_list[idx];
    WinRect r; wm_get_win_rect(win, &r);
    return (mx >= r.x1 && mx < r.x2 && my >= r.y1 && my < r.y1 + (int32_t)g_title_h);
}

static inline void wm_minimize_window(int idx) {
    if(idx<0||idx>=g_window_count)return;
    Window* win=g_window_list[idx];win->minimized=true;
    win->ghost_dragging=false;win->restore_drag_pending=false;win->resize_edges=0;
    if(g_focused_win_id==win->id){
        g_focused_win_id=-1;
        for(int s=g_window_count-1;s>=0;--s){auto* other=g_window_list[g_window_order[s]];
            if(other&&other->visible&&!other->minimized){g_focused_win_id=other->id;break;}}
    }
    g_dirty_min_x=0;g_dirty_min_y=0;g_dirty_max_x=fb->width-1;g_dirty_max_y=fb->height-1;g_needs_refresh=true;
}

static inline bool wm_is_in_close(int idx, int32_t mx, int32_t my) {
    if (idx < 0 || idx >= g_window_count) return false;
    Window* win = g_window_list[idx];
    WinRect r; wm_get_win_rect(win, &r);
    
    return window_control_hit(win->x,win->y-g_title_h,win->w,g_title_h,mx,my)==WINDOW_CLOSE;
}

static inline void wm_close_window(int idx) {
    if (idx < 0 || idx >= g_window_count) return;
    Window* win = g_window_list[idx];
    if (!win->is_system) {
        // Applications decide whether to exit, close one of several windows,
        // or ask the user to save. Keep the surface alive until they respond.
        win->close_pending = true;
        task_wake_console(win->owner_pid);
        return;
    }
    win->visible = false;
    ++g_resource_demand_generation; g_resources_requested=true;
    if (g_focused_win_id == win->id) {
        g_focused_win_id = -1;
        for (int s = g_window_count - 1; s >= 0; s--) {
            int o_idx = g_window_order[s];
            Window* w = g_window_list[o_idx];
            if (w && w->visible && !w->minimized) {
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
    win->shadow_enabled = true;
    win->is_system = is_system;
    win->type = type;
    win->owner_pid = owner;
    if (!is_system && g_current_task && g_current_task->pid==owner)
        resources::copy(win->executable,sizeof(win->executable),g_current_task->executable);
    
    uint64_t buf_bytes = (uint64_t)w * h * 4;
    uint64_t pages = (buf_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    win->canvas_width=w;win->canvas_height=h;win->min_w=160;win->min_h=100;
    if(is_system){win->native_resize=true;win->min_w=320;win->min_h=150;}
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
        // System renderers retain this backing pointer across resizing.
        uint64_t capacity=(uint64_t)(fb->width>(uint32_t)w?fb->width:w)*(fb->height>(uint32_t)h?fb->height:h)*4;
        win->buffer = (uint32_t*)kmalloc(capacity);
        if (win->buffer) memset(win->buffer, 0, capacity);
    }
    win->buffer_pitch = w * 4;
    
    int idx = g_window_count;
    g_window_list[idx] = win;
    g_window_order[idx] = idx;
    g_window_count++;
    if(!is_system&&g_current_task&&g_current_task->pid==owner&&g_current_task->focus_first_window){
        g_current_task->focus_first_window=false;wm_raise_window(idx);}
    ++g_resource_demand_generation; g_resources_requested=true;
    
    wm_damage_window_full(win);
    gpu_launch_window(owner,win->id);
    return win;
}

static inline Window* wm_find_window(int id) {
    for (int i = 0; i < g_window_count; i++)
        if (g_window_list[i] && g_window_list[i]->id == id) return g_window_list[i];
    return nullptr;
}

extern void tty_input(char c);
extern void term_write_string(char* s);

struct RetiredWindowSurface { uint64_t physical,pages; };
static RetiredWindowSurface g_retired_surfaces[64];
static unsigned g_retired_surface_count=0;
static Window* g_retired_windows=nullptr;
// Run only at the compositor's frame boundary: a preempted frame may still
// have a pointer into the previous surface, even on a single-core machine.
static void wm_collect_retired_surfaces() {
    TtyIrqGuard guard;
    ++g_compositor_frame;
    // Failed reset may leave a DMA read in flight. Pin retired client backing
    // as well as the quarantined destination until reboot, never recycle it.
    if(g_gpu_quarantine_sources)return;
    // Invalidate cached GGTT bindings before any backing pages can be reused,
    // including a new window reusing an identical pointer and allocation size.
    if(g_retired_surface_count || g_retired_windows)++g_gpu_surface_epoch;
    for(unsigned i=0;i<g_retired_surface_count;++i)
        for(uint64_t p=0;p<g_retired_surfaces[i].pages;++p)free_phys_page(g_retired_surfaces[i].physical+p*PAGE_SIZE);
    g_retired_surface_count=0;
    while(g_retired_windows){Window* win=g_retired_windows;g_retired_windows=win->retired_next;
        if(win->titlebar_cache){
            g_titlebar_cache_bytes-=win->titlebar_cache->capacity;
            kfree(win->titlebar_cache->pixels);kfree(win->titlebar_cache);
        }
        for(auto& slot:win->published)if(slot.allocation)kfree(slot.allocation);
        if(win->buffer_phys){for(uint64_t p=0;p<win->buffer_pages;++p)free_phys_page(win->buffer_phys+p*PAGE_SIZE);}
        else kfree(win->buffer);kfree(win);}
}
static bool wm_enable_native_resize(Window* win,int minw,int minh) {
    if(!win||win->is_system||!g_current_task||win->owner_pid!=g_current_task->pid||minw<32||minh<32||
       minw>(int)fb->width||minh>(int)(fb->height-g_title_h-g_taskbar_h))return false;
    uint64_t width=fb->width>3840?3840:fb->width,height=fb->height>2160?2160:fb->height;
    if(width<(uint64_t)win->canvas_width)width=win->canvas_width;if(height<(uint64_t)win->canvas_height)height=win->canvas_height;
    uint64_t pages=(width*height*4+PAGE_SIZE-1)/PAGE_SIZE;
    if(pages>win->buffer_pages){
        if(g_retired_surface_count==64)return false;
        uint64_t physical=alloc_phys_pages(pages);if(!physical)return false;
        auto* pixels=(uint32_t*)(HHDM+physical);memset(pixels,0,pages*PAGE_SIZE);
        memcpy(pixels,win->buffer,(uint64_t)win->canvas_width*win->canvas_height*4);
        TtyIrqGuard guard;
        if(g_retired_surface_count==64){for(uint64_t p=0;p<pages;++p)free_phys_page(physical+p*PAGE_SIZE);return false;}
        for(uint64_t p=0;p<pages;++p){uint64_t va=win->user_va+p*PAGE_SIZE;
            map_page_in(g_current_task->cr3,va,physical+p*PAGE_SIZE,PRESENT|WRITABLE|USER);
            __asm__ volatile("invlpg (%0)"::"r"(va):"memory");}
        g_retired_surfaces[g_retired_surface_count++]={win->buffer_phys,win->buffer_pages};
        win->buffer_phys=physical;win->buffer_pages=pages;win->buffer=pixels;
    }
    win->native_resize=true;win->min_w=minw;win->min_h=minh;return true;
}
static void wm_commit_resize(Window* win) {
    win->x=win->ghost_x;win->y=win->ghost_y;win->w=win->ghost_w;win->h=win->ghost_h;
    if(win->is_system){
        if(!win->maximized&&(win->type==WIN_TYPE_TERM||win->type==WIN_TYPE_LOG)){
            int width=win->w/8*8,height=win->h/10*10;
            if(win->resize_edges&RESIZE_LEFT)win->x+=win->w-width;
            if(win->resize_edges&RESIZE_TOP)win->y+=win->h-height;
            win->w=width;win->h=height;
        }
        win->canvas_width=win->w;win->canvas_height=win->h;win->buffer_pitch=win->w*4;
        memset(win->buffer,0,(uint64_t)win->w*win->h*4);
        if(win->type==WIN_TYPE_TERM){g_term_ox=win->x;g_term_oy=win->y;
            g_term_stride_pixels=win->w;g_term_max_cols=win->w/8;g_term_max_rows=win->h/10;
            if(g_term_max_cols>TERM_MATRIX_COLS)g_term_max_cols=TERM_MATRIX_COLS;
            if(g_term_scrolled_rows+g_term_max_rows>TERM_MATRIX_ROWS)g_term_scrolled_rows=TERM_MATRIX_ROWS-g_term_max_rows;
            if(term_x>=g_term_max_cols)term_x=g_term_max_cols-1;if(term_y>=g_term_max_rows)term_y=g_term_max_rows-1;
            term_sync_backbuffer();term_dirty_all();}
        else if(win->type==WIN_TYPE_LOG){g_log_win_cols=win->w/8;g_log_rows_visible=win->h/10;
            if(g_log_rows_visible>LOG_WIN_LINES)g_log_rows_visible=LOG_WIN_LINES;
            g_log_stride=win->w;g_log_win_backbuffer=win->buffer;log_last_rendered_scroll=-1;wm_log_render_to_buffer();}
        else if(win->type==WIN_TYPE_TEST)checker_render_surface(win->buffer,win->w,win->h,win->buffer_pitch);
    }else if(win->native_resize){wm_discard_pointer_events(win);win->resize_pending=true;task_wake_console(win->owner_pid);}
    win->resize_edges=0;win->chrome_drawn=false;
    g_dirty_min_x=0;g_dirty_min_y=0;g_dirty_max_x=fb->width-1;g_dirty_max_y=fb->height-1;g_needs_refresh=true;
}
static void wm_toggle_maximize(Window* win) {
    if(!win)return;
    ResizeRect target;
    if(win->maximized){target=win->restore_rect;win->maximized=false;}
    else {
        win->restore_rect={win->x,win->y,win->w,win->h};win->maximized=true;
        int width=fb->width>3840?3840:fb->width;
        int height=(int)fb->height-g_title_h-g_taskbar_h;if(height>2160)height=2160;
        target={0,(int)g_title_h,width,height};
    }
    win->ghost_dragging=false;win->restore_drag_pending=false;win->resize_edges=0;
    win->ghost_x=target.x;win->ghost_y=target.y;win->ghost_w=target.w;win->ghost_h=target.h;
    wm_commit_resize(win);
}
static void wm_event_coordinates(Window* win,int x,int y,WinEvent* event) {
    event->x=(int64_t)(x-win->x)*win->canvas_width/win->w;
    event->y=(int64_t)(y-win->y)*win->canvas_height/win->h;
}

static bool (*g_desktop_shortcut_hook)(char,uint32_t,bool)=nullptr;
static bool wm_terminal_paste(Window* win,bool repeat=false){
    if(!win||!win->is_system||win->type!=WIN_TYPE_TERM||!tty_raw_mode||g_terminal_foreground_pid)return false;
    if(!repeat)tty_input(22);
    return true;
}
static void wm_clipboard_paste(int target){
    if(target!=g_focused_win_id)return;
    Window* win=wm_find_window(target);
    if(!win||!win->visible||win->minimized)return;
    if(win->is_system){wm_terminal_paste(win);return;}
    WinEvent paste{};
    paste.type=WIN_EVENT_KEY_DOWN;
    paste.key='v';
    paste.mods=WIN_MOD_CTRL|WIN_MOD_TEXT_PASTE;
    wm_win_push_event(win,paste);
}
static inline void wm_dispatch_key(char c, uint32_t scancode, bool is_down, uint32_t mods,bool repeat=false) {
    g_kbd_modifiers = mods;
    if (!is_down) return;
    if(wm_taskbar_escape(c,mods))return;
    if(wm_clipboard_key(c,scancode,mods,repeat))return;
    if(g_menu_open&&c==27&&!mods){wm_menu_close();return;}
    if(g_desktop_shortcut_hook&&g_desktop_shortcut_hook(c,mods,repeat))return;
    Window* focused = wm_find_window(g_focused_win_id);
    if(focused&&focused->is_system&&focused->type==WIN_TYPE_TERM&&tty_raw_mode&&
        !g_terminal_foreground_pid&&(mods&WIN_MOD_CTRL)&&!(mods&(WIN_MOD_ALT|WIN_MOD_GUI))){
        if(c=='v'||c=='V'){wm_terminal_paste(focused,repeat);return;}
        if((mods&WIN_MOD_SHIFT)&&(c=='c'||c=='C')){if(!repeat)tty_input(3);return;}
    }
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
    // Non-text HID usages must not masquerade as printable ASCII in editors.
    ev.key = c ? (uint32_t)(uint8_t)c : scancode==0x4a?WIN_KEY_HOME:
        scancode==0x4d?WIN_KEY_END:scancode==0x4c?WIN_KEY_DELETE:0x2000u+scancode;
    ev.mods = mods;
    ev.delta = 0;
    wm_win_push_event(focused, ev);
}

#include "taskbar.h"

static bool wm_taskbar_popup_open(){return g_taskbar_popup!=0;}

// Called at catalog publication. Only the shell and window decorations use
// catalog assets. The old menu bounds were already damaged before the swap.
static void wm_damage_catalog_ui(){
    if(g_menu_open)wm_menu_dirty_rect();
    if(g_taskbar_popup)wm_taskbar_damage(false);
    wm_dirty_add(0,fb->width-1,fb->height-g_taskbar_h,fb->height-1);
    for(int i=0;i<g_window_count;++i)wm_damage_window_title(g_window_list[i]);
    g_needs_refresh=true;
}

// Translucent clients keep underlying animation exposed. Their title bars
// remain opaque; ghost outlines are overlays and never count as occluders.
static inline void wm_visible_client(Window* win, VisibleRegion* region) {
    region->count = 0;
    if (!win || !win->visible || win->minimized || !win->buffer || !fb) return;
    int32_t width = (int32_t)fb->width;
    int32_t height = fb->height > g_taskbar_h ? (int32_t)(fb->height-g_taskbar_h) : 0;
    VisibleRect client = visibility_clip(win->x,win->y,win->w,win->h,0,width,height);
    if (!visibility_valid(client)) return;
    region->rects[region->count++] = client;
    bool above = false;
    for (int s = 0; s < g_window_count; ++s) {
        Window* other = g_window_list[g_window_order[s]];
        if (other == win) { above = true; continue; }
        if (!above || !other || !other->visible || other->minimized || !other->buffer) continue;
        VisibleRect cover = visibility_clip(other->x,other->y,other->w,other->h,
                                             (int32_t)g_title_h,width,height);
        if(other->premultiplied && cover.y2>other->y)cover.y2=other->y;
        if (!visibility_subtract(region,cover)) {
            region->count = 1; region->rects[0] = client;
            return; // Conservative fallback for pathological fragmentation.
        }
        if (!region->count) return;
    }
}

static bool wm_client_rect_exposed(Window* win,int x,int y,int w,int h) {
    VisibleRegion region{};wm_visible_client(win,&region);
    VisibleRect target{x,y,x+w,y+h};
    for(unsigned i=0;i<region.count;++i)
        if(visibility_valid(visibility_intersect(region.rects[i],target)))return true;
    return false;
}

static uint64_t g_rain_render_count = 0;
static uint64_t g_rain_damage_pixels = 0;
static bool g_rain_surface_current = false;

// Keep a complete backing surface whenever exposed, but only request repaint
// for pixels people can see. The compositor remains independent of client type.
static inline void wm_rain_render(Window* win, const VisibleRegion& region,bool retained=false) {
    if (!win || !win->buffer || !region.count) return;
    uint64_t t0 = rdtsc_val();
    g_rain_surface_current = !g_gpu_compose_hook;
    if (g_rain_surface_current)
        rainbow_render_surface(win->buffer, (uint32_t)win->w, (uint32_t)win->h, win->buffer_pitch, rain_scroll, g_rainbow_wide);
    g_client_render_ticks = rdtsc_val() - t0;
    ++g_rain_render_count;
    if(retained)return; // A later single batch updates only exposed client pixels.
    if (!win->chrome_drawn) {
        wm_surface_damage_all(win);
        return;
    }
    if(g_gpu_compose_hook){
        VisibleRect bounds;
        if(gpu_animation_damage_bounds(region,&bounds)){
            wm_dirty_add(bounds.x1,bounds.x2-1,bounds.y1,bounds.y2-1);
            g_rain_damage_pixels+=(uint64_t)(bounds.x2-bounds.x1)*(bounds.y2-bounds.y1);
            return;
        }
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
    TtyIrqGuard guard;
    if (g_focused_win_id == id) {
        g_focused_win_id = -1;
        for (int s = g_window_count - 1; s >= 0; s--) {
            int o_idx = g_window_order[s];
            Window* w = g_window_list[o_idx];
            if (w && w->visible && !w->minimized && w->id != id) {
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
    // A compositor frame may have been preempted while holding this window.
    // Retire both metadata and backing until that frame has finished.
    win->visible=false;win->retired_next=g_retired_windows;g_retired_windows=win;
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
static inline void wm_overlay_present_unlocked(const OverlayFrame& frame,bool scene_changed=false) {
    if(!fb||!g_master_backbuffer)return;
    if(g_gpu_present_hook && g_desktop_ready){
        if(!scene_changed && !g_gpu_present_pending && gpu_overlay_equal(frame,g_presented_overlay))return;
        if(!g_gpu_present_hook(frame,wm_cursor_sprite(frame.cursor))){
            g_gpu_present_hook=nullptr;g_gpu_present_pending=false;
            g_dirty_min_x=0;g_dirty_min_y=0;g_dirty_max_x=fb->width-1;g_dirty_max_y=fb->height-1;
            g_needs_refresh=true;
            return;
        }
        g_gpu_present_pending=false;
        g_presented_overlay.cursor=frame.cursor;g_presented_overlay.count=frame.count;
        for(unsigned i=0;i<frame.count;++i)g_presented_overlay.outlines[i]=frame.outlines[i];
        return;
    }
    OverlayTarget target{(uint8_t*)fb->address,g_master_backbuffer,(int)fb->width,(int)fb->height,(int)fb->pitch};
    overlay_render_transition(target,g_presented_overlay,frame,wm_cursor_sprite(frame.cursor),scene_changed);
    // Remember exactly what these writes presented, never re-read live windows
    // here: input can change them between a scene snapshot and its final blit.
    g_presented_overlay.cursor=frame.cursor;
    g_presented_overlay.count=frame.count;
    for(unsigned i=0;i<frame.count;++i)g_presented_overlay.outlines[i]=frame.outlines[i];
    vram_fence();
    gpu_menu_present(rdtsc_val(),false);
}
static inline void wm_overlay_update_unlocked(int32_t x,int32_t y,uint8_t buttons) {
    OverlayFrame frame;wm_capture_overlay(&frame,{x,y,wm_cursor_edges(x,y),buttons});
    wm_overlay_present_unlocked(frame);
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
    // The initial terminal is launched through the userspace console host.
    g_win_test = wm_create_window("Visual Test", (int32_t)wm_win_x + 50, (int32_t)wm_win_y + 50 + (int32_t)g_title_h, (int32_t)test_w, (int32_t)test_h, true, true, WIN_TYPE_TEST);
    wm_test_render(g_win_test);
    g_win_rain = wm_create_window("Rainbow Animation", (int32_t)wm_win_x + 100, (int32_t)wm_win_y + 100 + (int32_t)g_title_h, (int32_t)test_w, (int32_t)test_h, true, true, WIN_TYPE_RAIN);
    g_win_log = wm_create_window("Boot Log", (int32_t)wm_win_x + 150, (int32_t)wm_win_y + 150 + (int32_t)g_title_h, (int32_t)test_w, (int32_t)test_h, true, true, WIN_TYPE_LOG);


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
// each completed blit presents/tracks its own snapshot; the final call reports completion.
static uint64_t wm_menu_pixel_key(int px,int py,int pw,int ph,int hover){
    uint64_t key=1469598103934665603ULL;
    auto hash=[&](uint64_t value){key=(key^value)*1099511628211ULL;};
    hash(fb->width);hash(fb->height);hash(fb->pitch);
    hash((uint32_t)px);hash((uint32_t)py);hash(pw);hash(ph);
    hash((uint32_t)hover);hash(g_font_scale_100);hash(g_decoration_epoch);
    return key;
}
static uint64_t wm_menu_pixel_key(){
    int px,py,pw,ph;wm_menu_geom(&px,&py,&pw,&ph);
    return wm_menu_pixel_key(px,py,pw,ph,wm_menu_hit_test(g_mouse_x,g_mouse_y));
}
// top_c/bot_c are the gradient endpoints; edge_c is the 1px border;
// shine=true draws a bright highlight row on top (glass effect).
static void wm_draw_button(uint32_t min_y,uint32_t max_y,int32_t bx, int32_t by, int32_t bw, int32_t bh,
                    uint32_t top_c, uint32_t bot_c,
                    uint32_t edge_c, bool shine,int32_t clip_left=0,int32_t clip_right=INT32_MAX) {
    int32_t left=bx>clip_left?bx:clip_left;if(left<0)left=0;
    int32_t right=bx+bw<clip_right?bx+bw:clip_right;
    if(right>(int32_t)fb->width)right=fb->width;
    if(left>=right)return;
    for (int32_t y = by; y < by + bh; y++) {
        if (y < (int32_t)min_y || y > (int32_t)max_y) continue;
        if (y < 0 || (uint32_t)y >= fb->height) continue;
        uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)y * fb->pitch);
        uint32_t t = (uint32_t)(y - by) * 255 / (bh > 1 ? (uint32_t)(bh - 1) : 1u);
        uint8_t rr  = (uint8_t)(((top_c>>16)&0xFF) + (int32_t)(((int32_t)((bot_c>>16)&0xFF)-(int32_t)((top_c>>16)&0xFF))*(int32_t)t/255));
        uint8_t gg  = (uint8_t)(((top_c>> 8)&0xFF) + (int32_t)(((int32_t)((bot_c>> 8)&0xFF)-(int32_t)((top_c>> 8)&0xFF))*(int32_t)t/255));
        uint8_t bb  = (uint8_t)(((top_c    )&0xFF) + (int32_t)(((int32_t)((bot_c    )&0xFF)-(int32_t)((top_c    )&0xFF))*(int32_t)t/255));
        uint32_t row_clr = 0xFF000000 | ((uint32_t)rr<<16) | ((uint32_t)gg<<8) | bb;
        if (shine && y == by) row_clr = 0xFFAABBEE;  // top shine row

        memset_32(drow+left,row_clr,right-left);

        // Left/right 1px edges
        if(bx>=left&&bx<right)drow[bx]=edge_c;
        if(bx+bw-1>=left&&bx+bw-1<right)drow[bx+bw-1]=edge_c;
    }
    // Top / bottom edge lines
    for(int32_t px=left;px<right;++px){
        auto edge_px = [&](int32_t ey, uint32_t ec) {
            if (ey < (int32_t)min_y || ey > (int32_t)max_y) return;
            if (ey < 0 || (uint32_t)ey >= fb->height) return;
            ((uint32_t*)(g_master_backbuffer + (uint64_t)ey * fb->pitch))[px] = ec;
        };
        edge_px(by,        edge_c);
        edge_px(by + bh - 1, 0xFF101828);
    }
}
// Stage 2.8: Start menu popup (topmost scene layer, below the cursor overlay)
static void wm_draw_start_menu(uint32_t min_y,uint32_t max_y,bool cache_only=false) {
    if(!g_menu_open||!fb)return;
    ResourceRead resources;
    int32_t px, py, pw, ph;
    wm_menu_geom(&px, &py, &pw, &ph);
    int32_t pad=wm_menu_pad(), header=wm_menu_header(), row_h=wm_menu_row();
    int32_t footer_top=py+ph-wm_menu_footer(), list_top=py+header+pad;
    int32_t char_h=(int32_t)wm_text_height(g_font_scale_100);
    int hover=wm_menu_hit_test(g_mouse_x,g_mouse_y);
    GpuPerfTimer menu_timer(g_gpu_perf.menu_ticks);
    auto render_menu=[&](){
        // Every primitive respects the current damage band and framebuffer bounds.
        auto rect=[&](int32_t x,int32_t y,int32_t w,int32_t h,uint32_t color,int radius=0) {
            for(int32_t yy=y;yy<y+h;++yy) {
                if(yy<(int32_t)min_y||yy>(int32_t)max_y||yy<0||yy>=(int32_t)fb->height)continue;
                int inset=0,edge=yy-y<h/2?yy-y:y+h-1-yy;
                if(edge<radius)inset=radius-edge;
                int32_t left=x+inset,right=x+w-inset;
                if(left<0)left=0;if(right>(int32_t)fb->width)right=fb->width;
                if(right>left)memset_32((uint32_t*)(g_master_backbuffer+(uint64_t)yy*fb->pitch)+left,color,right-left);
            }
        };
        auto text=[&](int32_t x,int32_t y,const char* label,uint32_t color,int32_t width) {
            if(y>=py&&y+char_h<=py+ph&&y+char_h>(int32_t)min_y&&y<=(int32_t)max_y)
                wm_draw_string(x,y,label,color,g_font_scale_100,width);
        };
        // Use the taskbar's navy surfaces, borders, and active-control gradient.
        wm_draw_button(min_y,max_y,px,py,pw,ph,0xFF23283D,0xFF181C30,0xFF303560,false);
        wm_draw_button(min_y,max_y,px+1,py+1,pw-2,header,0xFF3D4D80,0xFF28366A,0xFF5568B0,true);
        int32_t section_y=py+(header-char_h)/2;
        text(px+pad+8,section_y,"All apps",0xFFEEEEFF,pw-pad*2-8);
        char count[16];to_str((uint64_t)g_menu_app_count,count);
        int32_t count_w=wm_text_width(count,g_font_scale_100);
        text(px+pw-pad-8-count_w,section_y,count,0xFFB9C5EE,count_w);

        for(int i=0;i<g_menu_app_count;++i) {
            int32_t ry=list_top+i*row_h;
            if(ry+row_h>footer_top-pad)break;
            const auto& entry=g_resources_catalog->apps[i];
            bool hov=hover==i;
            if(hov)wm_draw_button(min_y,max_y,px+pad,ry,pw-pad*2,row_h-3,
                            0xFF3D4D80,0xFF28366A,0xFF5568B0,false);
            int32_t isz=wm_icon_size(row_h-10,14,128);
            int32_t ix=px+pad+8,iy=ry+(row_h-3-isz)/2;
            if(entry.icons.menu.pixels) {
                const Image& icon=entry.icons.menu;
                wm_draw_icon(icon,ix+(isz-(int32_t)icon.width)/2,iy+(isz-(int32_t)icon.height)/2,
                             px+pad,min_y,px+pw-pad,max_y+1);
            } else {
                uint32_t hash=0;for(const char* q=entry.app.id;*q;++q)hash=hash*31u+(uint8_t)*q;
                rect(ix,iy,isz,isz,g_rainbow_lut[hash%360],3);
            }
            int32_t ty=ry+(row_h-3-char_h)/2;
            text(ix+isz+10,ty,entry.app.name,entry.app.available?0xFFE6E8FF:0xFF8888AA,
                 pw-pad*2-isz-26);
        }
        if(!g_menu_app_count)
            text(px+pad+8,list_top+(row_h-char_h)/2,"No installed apps",0xFF9FA6CC,pw-pad*2-16);

        wm_draw_button(min_y,max_y,px+1,footer_top,pw-2,wm_menu_footer()-1,
                 0xFF252A42,0xFF1A1F35,0xFF303560,false);
        int32_t button_w=(pw-pad*3)/2,button_h=wm_menu_footer()-pad*2;
        for(int i=0;i<2;++i) {
            int action=i?MENU_SHUTDOWN:MENU_RESTART;
            int32_t bx=px+pad+i*(button_w+pad),by=footer_top+pad;
            bool hov=hover==action;
            wm_draw_button(min_y,max_y,bx,by,button_w,button_h,
                     hov?0xFF3D4D80:0xFF23283D,hov?0xFF28366A:0xFF181C30,
                     hov?0xFF5568B0:0xFF2C3258,hov);
            int32_t icon_size=wm_menu_scale(20,16),ix=bx+10,iy=by+(button_h-icon_size)/2;
            for(int y=0;y<icon_size;++y)for(int x=0;x<icon_size;++x) {
                int yy=iy+y,xx=ix+x;
                if(yy<(int32_t)min_y||yy>(int32_t)max_y||yy<0||yy>=(int32_t)fb->height||
                   xx<0||xx>=(int32_t)fb->width)continue;
                unsigned alpha=power_icon_coverage(i!=0,x,y,icon_size);
                if(!alpha)continue;
                uint32_t* pixel=(uint32_t*)(g_master_backbuffer+(uint64_t)yy*fb->pitch)+xx;
                uint32_t color=0xFF000000;
                for(unsigned shift=0;shift<24;shift+=8)
                    color|=(((((*pixel>>shift)&255)*(16-alpha)+0xEE*alpha+8)/16)<<shift);
                *pixel=color;
            }
            int32_t label_x=ix+icon_size+6;
            text(label_x,by+(button_h-char_h)/2,i?"Shut down":"Restart",0xFFE6E8FF,bx+button_w-label_x-6);
        }
    };
    // Absolute screen coordinates are shared with the existing text/icon
    // renderers. A reusable WB scratch surface keeps cache construction
    // independent of the WC scanout that will receive the finished menu.
    auto& cache=g_menu_pixel_cache;
    uint64_t bytes=(uint64_t)fb->pitch*fb->height;
    uint64_t current=wm_menu_pixel_key(px,py,pw,ph,hover);
    if(!cache.pixels || cache.capacity<bytes){
        if(cache_only)return;
        render_menu();
        ++g_gpu_perf.menu_renders;
    }else{
        if(cache.key!=current){
#if GPU_FRAME_DIAGNOSTICS
            GpuCpuPhaseTimer cost(gpu_phase_cost(GpuFrameTimeline::MenuBuild),wm_trace_cpu_cycles);
#endif
            uint64_t build_started=rdtsc_val();
            auto* menu_trace=gpu_menu_open_trace();
            if(menu_trace && !menu_trace->cache_start)menu_trace->cache_start=build_started;
            auto* destination=g_master_backbuffer;
            uint32_t saved_min=min_y,saved_max=max_y;
            int left=g_ui_clip_left,right=g_ui_clip_right,top=g_ui_clip_top,bottom=g_ui_clip_bottom;
            g_master_backbuffer=cache.pixels;
            min_y=py<0?0:py;max_y=py+ph-1;
            if(max_y>=fb->height)max_y=fb->height-1;
            g_ui_clip_left=px;g_ui_clip_right=px+pw;
            g_ui_clip_top=min_y;g_ui_clip_bottom=max_y+1;
            render_menu();
            if(menu_trace){
                menu_trace->cache_done=rdtsc_val();
                menu_trace->build_ticks+=menu_trace->cache_done-build_started;
            }
            g_master_backbuffer=destination;min_y=saved_min;max_y=saved_max;
            g_ui_clip_left=left;g_ui_clip_right=right;g_ui_clip_top=top;g_ui_clip_bottom=bottom;
            cache.key=current;++g_gpu_perf.menu_renders;
        }else ++g_gpu_perf.menu_reuses;
        if(cache_only)return;
        int left=px<0?0:px,right=px+pw;
        if(right>(int)fb->width)right=fb->width;
        int top=py>(int)min_y?py:min_y,bottom=py+ph;
        if(bottom>(int)max_y+1)bottom=max_y+1;
        for(int y=top;y<bottom && right>left;++y){
            uint64_t offset=(uint64_t)y*fb->pitch+left*4;
            memcpy_vram_sse_headless(g_master_backbuffer+offset,cache.pixels+offset,(right-left)*4);
        }
    }
}

static bool wm_retained_menu_ready(){
#if GPU_RETAINED_MENU
    return fb&&g_menu_open&&!g_clipboard_open&&!g_taskbar_popup&&g_menu_pixel_cache.pixels&&
        g_menu_pixel_cache.capacity>=(uint64_t)fb->pitch*fb->height&&
        g_menu_pixel_cache.key==wm_menu_pixel_key();
#else
    return false;
#endif
}
static uint64_t g_scene_compose_count=0,g_scene_compose_pixels=0;
static GpuScenePlan g_retained_animation_plan{};
struct RetainedAnimationLayer {GpuSceneClient client;bool shadow;int id;};
static RetainedAnimationLayer g_retained_animation_layers[MAX_WINDOWS];
static unsigned g_retained_animation_layer_count=0;
static uint64_t g_retained_animation_layout=0;
static bool g_retained_animation_alpha=false;
static uint64_t g_retained_animation_opaque_pixels=0;
#if GPU_ALPHA_REPAIR_BATCH && GPU_OPAQUE_RETAIN
static VisibleRect g_top_alpha_opaque[GpuOpaqueCoverage::capacity];
static unsigned g_top_alpha_opaque_count=0;
#endif
static uint64_t g_retained_menu_key=0;
static VisibleRect g_retained_menu_panel{};
static bool g_retained_scene_invalidated=false;
static uint64_t g_retained_popup_layout=0;
static uint64_t wm_retained_popup_layout(){
    if(!g_taskbar_popup)return 0;
    auto panel=wm_taskbar_panel(wm_taskbar_snapshot());
    uint64_t value=1469598103934665603ULL;
    auto hash=[&](uint64_t part){value=(value^part)*1099511628211ULL;};
    hash(g_taskbar_popup);hash(g_taskbar_group_id);hash(g_taskbar_anchor);
    hash(panel.x);hash(panel.y);hash(panel.w);hash(panel.h);hash(panel.count);
    return value;
}
static uint64_t wm_retained_animation_layout(){
    uint64_t value=1469598103934665603ULL;
    auto hash=[&](uint64_t part){value=(value^part)*1099511628211ULL;};
    hash(g_menu_open);hash(g_clipboard_open);hash(g_taskbar_popup);
    hash(wm_retained_popup_layout());
#if GPU_RETAINED_MENU
    if(g_menu_open){int x,y,w,h;wm_menu_geom(&x,&y,&w,&h);hash(x);hash(y);hash(w);hash(h);
        hash(g_decoration_epoch);hash(g_font_scale_100);}
#endif
    hash(g_title_h);hash(g_taskbar_h);hash(fb->width);hash(fb->height);
    for(int s=0;s<g_window_count;++s){
        const auto* win=g_window_list[g_window_order[s]];
        if(!win){hash(0);continue;}
        hash(win->id);hash(win->visible);hash(win->minimized);
        hash((uint32_t)win->x);hash((uint32_t)win->y);hash(win->w);hash(win->h);
        hash(win->premultiplied);hash(win->shadow_enabled);hash(win->maximized);
    }
    return value;
}
static bool wm_prepare_retained_animation(Window* animation){
#if GPU_RETAINED_MENU
    if(g_menu_open&&!wm_retained_menu_ready()&&!g_clipboard_open&&!g_taskbar_popup)
        wm_draw_start_menu(0,0,true);
#endif
    if(!animation||!animation->buffer||!animation->visible||animation->minimized||
       !animation->chrome_drawn||animation->premultiplied||
       animation->canvas_width!=animation->w||animation->canvas_height!=animation->h||
       (g_menu_open&&!wm_retained_menu_ready())||g_clipboard_open||!g_gpu_compose_hook)return false;
    bool above=false,found=false,maximized=false;
    VisibleRect covers[MAX_WINDOWS+1];unsigned count=0;
    GpuSceneClient client{};
    g_retained_animation_layer_count=0;g_retained_animation_alpha=false;
    g_retained_animation_opaque_pixels=0;
    {
    TtyIrqGuard guard;
    if(g_menu_open&&!wm_retained_menu_ready())return false;
    g_retained_menu_key=g_menu_open?wm_menu_pixel_key():0;
    g_retained_popup_layout=wm_retained_popup_layout();
    // The app picker is opaque. Keep its pixels in both retained scanouts
    // and exclude them from Rainbow AND every overlying alpha layer. Hover,
    // scrolling and content damage still use the ordinary popup renderer.
    if(g_taskbar_popup){
        auto panel=wm_taskbar_panel(wm_taskbar_snapshot());
        if(!panel.count)return false;
        covers[count++]={panel.x,panel.y,panel.x+panel.w,panel.y+panel.h};
    }
    if(g_menu_open){int x,y,w,h;wm_menu_geom(&x,&y,&w,&h);
        g_retained_menu_panel=visibility_intersect({x,y,x+w,y+h},{0,0,(int)fb->width,(int)fb->height});}
    for(int s=0;s<g_window_count;++s){
        auto* win=g_window_list[g_window_order[s]];
        if(!win||!win->visible||win->minimized)continue;
        if(win->maximized)maximized=true;
        if(win==animation){above=true;found=true;continue;}
        if(!above)continue;
        WinRect r;wm_get_win_rect(win,&r);
        if(!r.visible)continue;
        auto outer=VisibleRect{r.x1,r.y1,r.x2,r.y2};
        covers[count++]=win->premultiplied?VisibleRect{r.x1,r.y1,r.x2,r.y1+(int)g_title_h}:outer;
        auto& layer=g_retained_animation_layers[g_retained_animation_layer_count++];
        layer.id=win->id;
        layer.client={outer,{r.x1,r.y1+(int)g_title_h,r.x2,r.y2},win->buffer,
            (uint32_t)win->canvas_width,(uint32_t)win->canvas_height,win->buffer_pitch,
            true,false,!win->maximized,win->premultiplied,0};
        const auto& published=win->published[win->published_slot];
        if(published.pixels&&!g_gpu_quarantine_sources){
            layer.client.pixels=published.pixels;layer.client.width=published.width;
            layer.client.height=published.height;layer.client.pitch=published.width*4;
            layer.client.revision=published.revision;
        }
        layer.shadow=win->shadow_enabled&&!win->maximized;
        if(win->premultiplied){
            if(!g_gpu_render_hook)return false;
            g_retained_animation_alpha=true;
        }
    }
    // The ordinary HUD path may paint over a nonmaximized client in these rows.
    if(!found||(!maximized&&animation->y<60))return false;
    WinRect r;wm_get_win_rect(animation,&r);
    client={{r.x1,r.y1,r.x2,r.y2},
        {r.x1,r.y1+(int)g_title_h,r.x2,r.y2},animation->buffer,
        (uint32_t)animation->w,(uint32_t)animation->h,(uint32_t)animation->w*4,
        true,true,!animation->maximized,false,0};
    g_retained_animation_layout=wm_retained_animation_layout();
    }
    g_retained_animation_plan.scroll=rain_scroll;
    g_retained_animation_plan.rainbow_colors=g_rainbow_wide;
    if(!gpu_retained_animation_plan(&g_retained_animation_plan,client,
        {0,0,(int)fb->width,(int)(fb->height-g_taskbar_h)},covers,count))return false;
#if GPU_OPAQUE_RETAIN
    // Published sources cannot be reused during this compositor frame. Scan
    // outside the IRQ guard; mutable/unpublished sources prove no coverage.
    static GpuOpaqueCoverage opaque_cache[MAX_WINDOWS];
    static VisibleRect opaque_covers[MAX_WINDOWS*GpuOpaqueCoverage::capacity];
    unsigned opaque_count=0;
#if GPU_ALPHA_REPAIR_BATCH
    g_top_alpha_opaque_count=0;
#endif
    for(unsigned s=0;s<g_retained_animation_layer_count;++s){
        const auto& c=g_retained_animation_layers[s].client;
        if(!c.premultiplied)continue;
        auto& coverage=opaque_cache[s];
        uint64_t started=GpuPerfTimer::now();
        if(coverage.update(c.pixels,c.width,c.height,c.pitch,c.revision)){
            ++g_gpu_perf.opaque_scans;g_gpu_perf.opaque_scan_ticks+=GpuPerfTimer::now()-started;
        }
        unsigned added=coverage.mapped(c.client,c.resize_grip?12:0,opaque_covers+opaque_count);
#if GPU_ALPHA_REPAIR_BATCH
        if(s+1==g_retained_animation_layer_count){
            g_top_alpha_opaque_count=added;
            for(unsigned i=0;i<added;++i)g_top_alpha_opaque[i]=opaque_covers[opaque_count+i];
        }
#endif
        opaque_count+=added;
    }
    if(opaque_count){
        uint64_t before=0,after=0;
        for(unsigned i=0;i<g_retained_animation_plan.count;++i){auto r=g_retained_animation_plan.copies[i].destination;
            before+=(uint64_t)(r.x2-r.x1)*(r.y2-r.y1);}
        if(!gpu_retained_opaque_cutouts(&g_retained_animation_plan,opaque_covers,opaque_count))return false;
        for(unsigned i=0;i<g_retained_animation_plan.count;++i){auto r=g_retained_animation_plan.copies[i].destination;
            after+=(uint64_t)(r.x2-r.x1)*(r.y2-r.y1);}
        g_retained_animation_opaque_pixels=before-after;
    }
    // A publication can change coverage while normal repairs block. The
    // second preparation must detect changed cutouts and repaint the scene.
    auto hash=[&](uint64_t part){g_retained_animation_layout=(g_retained_animation_layout^part)*1099511628211ULL;};
    hash(opaque_count);
    for(unsigned i=0;i<opaque_count;++i){const auto& r=opaque_covers[i];
        hash((uint32_t)r.x1);hash((uint32_t)r.y1);hash((uint32_t)r.x2);hash((uint32_t)r.y2);}
#endif
    static GpuScenePlan alpha_plan{};
    for(unsigned s=0;s<g_retained_animation_layer_count;++s){
        const auto& c=g_retained_animation_layers[s].client;
        if(c.premultiplied&&!gpu_retained_alpha_plan(&alpha_plan,c,g_retained_animation_plan))return false;
    }
    return true;
}
static void wm_retained_animation_grip(const GpuSceneClient& client,VisibleRect clip){
    if(!client.resize_grip)return;
    for(int bottom=1;bottom<12;++bottom){
        int y=client.client.y2-1-bottom;
        if(y<clip.y1||y>=clip.y2)continue;
        auto* row=(uint32_t*)(g_master_backbuffer+(uint64_t)y*fb->pitch);
        for(int right=1;right<12;++right){
            int x=client.client.x2-1-right;
            if(x>=clip.x1&&x<clip.x2&&(right+bottom==6||right+bottom==10||right+bottom==14))row[x]=0xff81728f;
        }
    }
}
#if GPU_ALPHA_REPAIR_BATCH
static GpuDeferredAlpha g_deferred_alpha{};
static int g_deferred_alpha_id=0;
static void wm_begin_alpha_repairs(bool retained){
    g_deferred_alpha.clear();g_deferred_alpha_id=0;
    if(!retained||!g_retained_animation_layer_count)return;
    const auto& top=g_retained_animation_layers[g_retained_animation_layer_count-1];
    g_deferred_alpha_id=top.id;g_deferred_alpha.begin(top.client,true);
}
static bool wm_alpha_repairs_ready(){
    if(!g_deferred_alpha.count)return !g_deferred_alpha.failed;
    if(!g_retained_animation_layer_count)return false;
    const auto& top=g_retained_animation_layers[g_retained_animation_layer_count-1];
    return top.id==g_deferred_alpha_id&&g_deferred_alpha.ready(top.client,g_retained_animation_plan);
}
static void wm_cancel_alpha_repairs(){
    if(g_deferred_alpha.count)++g_gpu_perf.alpha_repair_cancels;
    g_deferred_alpha.clear();
}
#endif
static void wm_copy_cached_menu(VisibleRect r){
    for(int row=r.y1;row<r.y2;++row){
        uint64_t offset=(uint64_t)row*fb->pitch+r.x1*4;
        memcpy_vram_sse_headless(g_master_backbuffer+offset,g_menu_pixel_cache.pixels+offset,(r.x2-r.x1)*4);
    }
}

// Retained tray repairs share the full taskbar's renderer, including exact
// gradients, borders and text. Only this opaque strip is touched; no cached
// scanout pixels or alpha backgrounds are read back.
static void wm_draw_taskbar_tray(VisibleRect clip,bool background=true){
    int32_t tb_y=(int32_t)fb->height-(int32_t)g_taskbar_h;
    clip=visibility_intersect(clip,{wm_tray_damage_left(),tb_y,(int)fb->width,(int)fb->height});
    if(!visibility_valid(clip))return;
    ResourceRead resource_read;
    int32_t old_left=g_ui_clip_left,old_right=g_ui_clip_right;
    int32_t old_top=g_ui_clip_top,old_bottom=g_ui_clip_bottom;
    g_ui_clip_left=clip.x1;g_ui_clip_right=clip.x2;g_ui_clip_top=clip.y1;g_ui_clip_bottom=clip.y2;
    uint32_t min_y=clip.y1,max_y=clip.y2-1;
    int32_t tbr1=clip.y1,tbr2=clip.y2-1;
    const int32_t sb_y=tb_y+5,sb_h=(int32_t)g_taskbar_h-10;
    if(background){
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
            memset_32(drow+clip.x1,clr,clip.x2-clip.x1);
        }

    }
    // System tray: shared by full taskbar rendering and exact retained repairs.
    const int32_t TRAY_W = wm_tray_w();
    int32_t tray_x = (int32_t)fb->width - TRAY_W - 6;
    int32_t tray_y = sb_y, tray_h = sb_h;

    wm_draw_button(min_y,max_y,tray_x, tray_y, TRAY_W, tray_h,
            0xFF252a42, 0xFF1a1f35,
            0xFF303560, false,clip.x1,clip.x2);

    // Clock: time on top, date below (like the Windows taskbar). Only reads the cached g_clock.
    {
        uint32_t clock_scale = wm_clock_scale();
        int32_t fps_width=wm_fps_width();
        int32_t clock_x=tray_x+fps_width+g_taskbar_h,clock_width=wm_clock_width();
        int size=g_taskbar_h-16;if(size>28)size=28;if(size<14)size=14;
        int icon_x=wm_volume_tray_x()+(g_taskbar_h-size)/2;
        wm_draw_volume_icon(icon_x,tray_y+(tray_h-size)/2,size,g_audio_mixer.value.percent,g_audio_mixer.value.muted,min_y,max_y);
        char fps[16];to_str(g_desktop_fps,fps);
        resources::append(fps,sizeof(fps)," FPS");
        int32_t ch = wm_text_height(clock_scale);
        int32_t fps_x=tray_x+(fps_width-wm_text_width(fps,clock_scale))/2;
        wm_draw_string(fps_x,tray_y+(tray_h-ch)/2,fps,0xFFE6E8FF,clock_scale);
        char tbuf[16], dbuf[16];
        int tl, dl = 0;
        if (g_clock_valid) {
            tl = rtc_format_time(g_clock, tbuf, CLOCK_SHOW_SECONDS != 0);
            dl = rtc_format_date(g_clock, dbuf);
        } else {
            tbuf[0] = '-'; tbuf[1] = '-'; tbuf[2] = ':'; tbuf[3] = '-'; tbuf[4] = '-'; tbuf[5] = 0;
            tl = 5;
        }
        bool two_lines = dl > 0 && tray_h >= 2 * ch + 2;
        int32_t ty0 = two_lines ? tray_y + (tray_h - (2 * ch + 2)) / 2
                                : tray_y + (tray_h - ch) / 2;
        wm_draw_string((uint32_t)(clock_x + (clock_width - wm_text_width(tbuf, clock_scale)) / 2), (uint32_t)ty0,
                       tbuf, 0xFFE6E8FF, clock_scale);
        if (two_lines)
            wm_draw_string((uint32_t)(clock_x + (clock_width - wm_text_width(dbuf, clock_scale)) / 2), (uint32_t)(ty0 + ch + 2),
                           dbuf, 0xFF9FA6CC, clock_scale);
    }

    // Separator between task area and tray
    for (int32_t y = sb_y + 4; y < sb_y + sb_h - 4; y++) {
        if (y < (int32_t)min_y || y > (int32_t)max_y) continue;
        if (y < 0 || (uint32_t)y >= fb->height) continue;
        uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)y * fb->pitch);
        int32_t sep = tray_x - 6;
        if(sep>=clip.x1&&sep<clip.x2)drow[sep]=0xFF404870;
        if(sep+1>=clip.x1&&sep+1<clip.x2)drow[sep+1]=0xFF1a1f35;
    }
    g_ui_clip_left=old_left;g_ui_clip_right=old_right;
    g_ui_clip_top=old_top;g_ui_clip_bottom=old_bottom;
}

static bool wm_repair_retained_foreground(VisibleRect r){
#if GPU_EXACT_REPAIRS
    // Clock/FPS damage is independent of the animation and window stack.
    // Avoid rebuilding every task button before the critical BCS/RCS chain.
    bool tray_clear=!g_menu_open;
    if(g_menu_open&&wm_retained_menu_ready()){
        int x,y,w,h;wm_menu_geom(&x,&y,&w,&h);
        tray_clear=y+h<=(int)(fb->height-g_taskbar_h);
    }
    bool popup_above_tray=!g_taskbar_popup;
    if(g_taskbar_popup){auto panel=wm_taskbar_panel(wm_taskbar_snapshot());
        popup_above_tray=panel.y+panel.h<=(int)(fb->height-g_taskbar_h);}
    if(tray_clear&&!g_clipboard_open&&popup_above_tray&&
       r.x1>=wm_tray_damage_left()&&r.x2<=(int)fb->width&&
       r.y1>=(int)(fb->height-g_taskbar_h)&&r.y2<=(int)fb->height&&visibility_valid(r)){
        GpuPerfTimer timer(g_gpu_perf.tray_repair_ticks);
        wm_draw_taskbar_tray(r);
        ++g_gpu_perf.tray_repair_rects;
        g_gpu_perf.tray_repair_pixels+=(uint64_t)(r.x2-r.x1)*(r.y2-r.y1);
        return true;
    }
#endif
    // The opaque popup is replayed in full after the retained animation and
    // alpha chain. Its own content damage needs no reconstruction underneath.
    // Accept only the exact popup geometry captured by this retained frame.
    if(g_retained_popup_layout&&g_retained_popup_layout==wm_retained_popup_layout()){
        auto panel=wm_taskbar_panel(wm_taskbar_snapshot());
        if(visibility_valid(r)&&r.x1>=panel.x&&r.y1>=panel.y&&
           r.x2<=panel.x+panel.w&&r.y2<=panel.y+panel.h)return true;
    }
    // Shell bands can contain other decorations with wider repaint rules.
    if(r.y1<60||r.y2>(int)(fb->height-g_taskbar_h))return false;
#if GPU_RETAINED_MENU
    const auto panel=g_retained_menu_panel;
    if(g_retained_menu_key&&wm_retained_menu_ready()&&g_menu_pixel_cache.key==g_retained_menu_key&&
       panel.x1<=r.x1&&panel.x2>=r.x2&&panel.y1<=r.y1&&panel.y2>=r.y2){
        wm_copy_cached_menu(r);
        ++g_gpu_perf.menu_cache_repairs;
        return true;
    }
#endif
#if GPU_ALPHA_REPAIR_BATCH && GPU_OPAQUE_RETAIN
    // Closing the menu can expose a proven fully opaque patch of an alpha
    // window. The destination underneath contributes zero to its final color.
    if(!g_retained_menu_key&&!g_menu_open&&!g_taskbar_popup&&g_deferred_alpha.active&&
       g_deferred_alpha.append_opaque(r,g_top_alpha_opaque,g_top_alpha_opaque_count,g_retained_animation_plan)){
        ++g_gpu_perf.opaque_repair_rects;
        return true;
    }
#endif
    return false;
}
static bool wm_draw_retained_animation([[maybe_unused]] uint64_t repair_menu_key){
    g_retained_scene_invalidated=false;
    const auto& plan=g_retained_animation_plan;
    if(!g_gpu_compose_hook)return false;
    const auto& client=plan.clients[0];
    // The first layer's shadow sees exactly the just-rendered procedural
    // background. Supply WB pixels instead of reading them back from WC RAM.
    alignas(64) static uint32_t background_row[8192];
    bool row_available=fb->width<=8192;
    if(row_available){
        int64_t phase=((int64_t)plan.scroll-client.client.x1)%720;
        if(phase<0)phase+=720;
        for(unsigned x=0;x<fb->width;++x){
            background_row[x]=0xff000000u|plan.rainbow_colors[phase];
            if(++phase==720)phase=0;
        }
    }
    // Tall exposed strips cost a complete BCS submission before alpha can
    // start. All scanlines of Rainbow have identical colors. Stream the hot
    // row directly into the retained hidden buffer for these layouts.
    bool stream=row_available && g_retained_popup_layout;
    if(row_available && g_retained_animation_alpha){
        for(unsigned i=0;i<g_retained_animation_layer_count;++i){
            const auto& layer=g_retained_animation_layers[i].client;
            if(layer.premultiplied && layer.client.x2-layer.client.x1<(int)fb->width &&
               layer.client.y2-layer.client.y1>=(int)fb->height/2)stream=true;
        }
    }
    if(stream){
        for(unsigned i=0;i<plan.count;++i){
            auto r=plan.copies[i].destination;
            for(int y=r.y1;y<r.y2;++y)
                memcpy_vram_sse_headless(g_master_backbuffer+(uint64_t)y*fb->pitch+r.x1*4,
                    background_row+r.x1,(r.x2-r.x1)*4);
        }
        vram_fence();
    }else if(!g_gpu_compose_hook(plan))return false;
    for(unsigned i=0;i<plan.count;++i){
        auto clip=plan.copies[i].destination;
        g_gpu_perf.retained_animation_pixels+=(uint64_t)(clip.x2-clip.x1)*(clip.y2-clip.y1);
        wm_retained_animation_grip(client,clip);
    }
    static GpuScenePlan alpha_plan{};
    bool clean_background=row_available,used_background_row=false;
    for(unsigned s=0;s<g_retained_animation_layer_count;++s){
        const auto& layer=g_retained_animation_layers[s];
        const auto& c=layer.client;
        for(unsigned i=0;i<plan.count;++i){
            auto clip=plan.copies[i].destination;
            if(layer.shadow){
                auto plain=clip;
                if(clean_background){
                    // CPU grip ink differs from the palette in the last rows.
                    if(client.resize_grip&&plain.y2>client.client.y2-12)plain.y2=client.client.y2-12;
                    if(visibility_valid(plain)){
                        window_shadow_draw(
                            {g_master_backbuffer,(int)fb->width,(int)(fb->height-g_taskbar_h),(int)fb->pitch,background_row},
                            c.outer,plain);
                        used_background_row=true;
                    }
                    if(clip.y1<plain.y2)clip.y1=plain.y2;
                }
                if(visibility_valid(clip))window_shadow_draw(
                    {g_master_backbuffer,(int)fb->width,(int)(fb->height-g_taskbar_h),(int)fb->pitch},
                    c.outer,clip);
            }
        }
        if(layer.shadow||c.premultiplied)clean_background=false;
        if(c.premultiplied){
            if(!gpu_retained_alpha_plan(&alpha_plan,c,plan))return false;
#if GPU_ALPHA_REPAIR_BATCH
            if(layer.id==g_deferred_alpha_id&&!g_deferred_alpha.combine(&alpha_plan))return false;
#endif
            if(alpha_plan.count&&(!g_gpu_render_hook||!g_gpu_render_hook(alpha_plan)))return false;
            for(unsigned i=0;i<plan.count;++i){
                auto clip=visibility_intersect(plan.copies[i].destination,c.client);
                if(!visibility_valid(clip))continue;
                wm_retained_animation_grip(c,clip);
            }
        }
    }
#if GPU_RETAINED_MENU
    auto menu_stable=[&](){TtyIrqGuard guard;
        return g_retained_menu_key?(wm_retained_menu_ready()&&wm_menu_pixel_key()==g_retained_menu_key):!g_menu_open;};
    auto finish_with_closing_menu=[&](){
        const auto panel=g_retained_menu_panel;
        {
            TtyIrqGuard guard;
            if(!g_retained_menu_key||g_menu_open||g_clipboard_open||g_taskbar_popup||
               g_menu_pixel_cache.key!=g_retained_menu_key||!visibility_valid(panel))return false;
            // Keep the in-flight frame complete. Repair the exposed area on the
            // next frame, even if this frame already took the close damage.
            wm_dirty_add(panel.x1,panel.x2-1,panel.y1,panel.y2-1);
            g_needs_refresh=true;
        }
#if GPU_FRAME_DIAGNOSTICS
        GpuCpuPhaseTimer cost(gpu_phase_cost(GpuFrameTimeline::MenuReplay),wm_trace_cpu_cycles);
#endif
        GpuPerfTimer timer(g_gpu_perf.menu_ticks);
        wm_copy_cached_menu(panel);
        g_gpu_perf.retained_menu_pixels+=(uint64_t)(panel.x2-panel.x1)*(panel.y2-panel.y1);
        ++g_gpu_perf.retained_menu_close_deferrals;
        return true;
    };
    bool repaint_panel=repair_menu_key!=g_retained_menu_key;
    bool closing_menu_replayed=false;
    if(!menu_stable()){
        // A new opaque panel can cover the finished frame. Moving or closing
        // the old one exposes pixels that may need a complete scene repair.
        if(g_menu_open&&!g_clipboard_open&&!g_taskbar_popup){
            wm_draw_start_menu(0,0,true);
            TtyIrqGuard guard;
            int x,y,w,h;wm_menu_geom(&x,&y,&w,&h);
            auto panel=visibility_intersect({x,y,x+w,y+h},{0,0,(int)fb->width,(int)fb->height});
            const auto& old=g_retained_menu_panel;
            bool compatible=!g_retained_menu_key||(panel.x1==old.x1&&panel.y1==old.y1&&panel.x2==old.x2&&panel.y2==old.y2);
            if(compatible&&wm_retained_menu_ready()){
                g_retained_menu_key=g_menu_pixel_cache.key;
                g_retained_menu_panel=panel;
                repaint_panel=true;
            }
        }
        if(!menu_stable()){
            closing_menu_replayed=finish_with_closing_menu();
            if(!closing_menu_replayed){g_retained_scene_invalidated=true;return true;}
        }
    }
    if(g_retained_menu_key&&!closing_menu_replayed){
#if GPU_FRAME_DIAGNOSTICS
        GpuCpuPhaseTimer cost(gpu_phase_cost(GpuFrameTimeline::MenuReplay),wm_trace_cpu_cycles);
#endif
        GpuPerfTimer timer(g_gpu_perf.menu_ticks);
        if(repaint_panel){
            auto panel=g_retained_menu_panel;
            wm_copy_cached_menu(panel);
            g_gpu_perf.retained_menu_pixels+=(uint64_t)(panel.x2-panel.x1)*(panel.y2-panel.y1);
        }else g_gpu_perf.retained_menu_pixels+=gpu_retained_overlay_replay(plan,g_retained_menu_panel,wm_copy_cached_menu);
        ++g_gpu_perf.retained_menu_frames;++g_gpu_perf.menu_reuses;
    }
    if(!closing_menu_replayed&&!menu_stable()&&!finish_with_closing_menu()){
        g_retained_scene_invalidated=true;
        return true;
    }
#endif
    // A popup may open, move or close while a GPU wait yields. The old
    // cutout cannot be presented as the new scene; use the existing retry.
    if(g_retained_popup_layout!=wm_retained_popup_layout()){
        g_retained_scene_invalidated=true;
        return true;
    }
    if(g_retained_popup_layout){
        // Popup pixels are topmost even when an earlier scene repair or a
        // deferred alpha draw touched their retained cutout. Replay the whole
        // panel with its own clip, independent of the last repair rectangle.
        ResourceRead resource_read;
        auto panel=wm_taskbar_panel(wm_taskbar_snapshot());
        int left=g_ui_clip_left,right=g_ui_clip_right,top=g_ui_clip_top,bottom=g_ui_clip_bottom;
        g_ui_clip_left=panel.x;g_ui_clip_right=panel.x+panel.w;
        g_ui_clip_top=panel.y;g_ui_clip_bottom=panel.y+panel.h;
        auto& scratch=g_menu_pixel_cache;
        bool buffered=scratch.pixels && scratch.capacity>=(uint64_t)fb->pitch*fb->height;
        auto* destination=g_master_backbuffer;
        if(buffered){
            scratch.key=0;
            g_master_backbuffer=scratch.pixels;
        }
        wm_taskbar_render_popup(panel.y,panel.y+panel.h-1);
        g_master_backbuffer=destination;
        if(buffered){
            auto bounds=visibility_intersect({panel.x,panel.y,panel.x+panel.w,panel.y+panel.h},
                {0,0,(int)fb->width,(int)fb->height});
            if(visibility_valid(bounds))wm_copy_cached_menu(bounds);
            vram_fence();
        }
        g_ui_clip_left=left;g_ui_clip_right=right;g_ui_clip_top=top;g_ui_clip_bottom=bottom;
        if(g_retained_popup_layout!=wm_retained_popup_layout()){
            g_retained_scene_invalidated=true;
            return true;
        }
    }
    ++g_gpu_perf.retained_animation_frames;
#if GPU_ALPHA_REPAIR_BATCH
    if(g_deferred_alpha.count){++g_gpu_perf.alpha_repair_frames;g_gpu_perf.alpha_repair_rects+=g_deferred_alpha.count;}
    g_deferred_alpha.clear();
#endif
    g_gpu_perf.opaque_retained_pixels+=g_retained_animation_opaque_pixels;
    if(g_retained_animation_alpha)++g_gpu_perf.retained_alpha_frames;
    if(used_background_row)++g_gpu_perf.shadow_row_frames;
    g_gpu_present_pending=true;
    return true;
}
static uint64_t g_gpu_fullscreen_fast_count=0;
static uint64_t g_gpu_scaled_fast_count=0;
static uint64_t g_gpu_retained_fast_count=0;
static uint64_t g_gpu_ordered_scene_count=0;
// Keep translucency on SIMD at every supported size: the former size-triggered
// switch to UC RCS blending caused a visible latency cliff. Opaque scaling
// remains on RCS. Mutable only for explicit driver regression fixtures.
static uint64_t g_cpu_alpha_pixel_limit=GPU_RCS_ALPHA?0:UINT64_MAX;
static uint64_t wm_gpu_perf_layout_signature(){
    TtyIrqGuard guard;
    uint64_t current=1469598103934665603ULL;
    auto hash=[&](uint64_t value){current=(current^value)*1099511628211ULL;};
    hash(g_menu_open);hash(g_clipboard_open);hash(g_taskbar_popup);hash(g_focused_win_id);
    for(int s=0;s<g_window_count;++s){
        auto* win=g_window_list[g_window_order[s]];
        hash(win->id);hash(win->visible&&!win->minimized);
        hash((uint32_t)win->x);hash((uint32_t)win->y);hash(win->w);hash(win->h);
        hash(win->premultiplied);hash(win->maximized);
    }
    return current;
}
#if GPU_WORKLOAD
// At most once per stable-layout observation; never print while holding IRQs.
static void wm_gpu_geometry_sample(uint64_t signature){
    struct Entry {
        int id;char title[40];GpuOcclusionLayer layer;
        unsigned width,height;bool rainbow,maximized;
    };
    static Entry entries[MAX_WINDOWS]{};unsigned count=0,rainbow=MAX_WINDOWS;
    {
        TtyIrqGuard guard;
        if(wm_gpu_perf_layout_signature()!=signature)return;
        for(int s=0;s<g_window_count&&count<MAX_WINDOWS;++s){
            auto* win=g_window_list[g_window_order[s]];
            if(!win||!win->visible||win->minimized)continue;
            WinRect r;wm_get_win_rect(win,&r);if(!r.visible)continue;
            auto& e=entries[count];e.id=win->id;
            unsigned n=0;for(;n<sizeof(e.title)-1&&win->title[n];++n)e.title[n]=win->title[n];e.title[n]=0;
            e.layer={{r.x1,r.y1,r.x2,r.y2},{r.x1,r.y1+(int)g_title_h,r.x2,r.y2},win->premultiplied};
            e.width=win->canvas_width;e.height=win->canvas_height;
            const auto& published=win->published[win->published_slot];
            if(published.pixels&&!g_gpu_quarantine_sources){e.width=published.width;e.height=published.height;}
            e.maximized=win->maximized;e.rainbow=win->is_system&&win->type==WIN_TYPE_RAIN;
            if(e.rainbow)rainbow=count;
            ++count;
        }
    }
    if(rainbow==MAX_WINDOWS)return;
    static GpuOcclusionLayer layers[MAX_WINDOWS]{};
    static GpuOcclusionMetric metrics[MAX_WINDOWS]{};
    unsigned above=count-rainbow-1;
    for(unsigned i=0;i<above;++i)layers[i]=entries[rainbow+1+i].layer;
    bool valid=gpu_occlusion_metrics(entries[rainbow].layer.client,
        {0,0,(int)fb->width,(int)(fb->height-g_taskbar_h)},layers,above,metrics);
    for(unsigned i=rainbow;i<count;++i){
        const auto& e=entries[i];char line[512];char* end=line;
        auto text=[&](const char* value){while(*value&&end<line+sizeof(line)-1)*end++=*value++;};
        auto number=[&](uint64_t value){char digits[24];to_str(value,digits);text(digits);};
        auto coordinate=[&](int value){if(value<0){text("-");number(-(int64_t)value);}else number(value);};
        auto rect=[&](const char* label,VisibleRect r){text(label);coordinate(r.x1);text(",");coordinate(r.y1);
            text(",");coordinate(r.x2);text(",");coordinate(r.y2);};
        text("GPU geometry: layout=");number(signature);text(" order=");number(i-rainbow);
        text(" id=");number(e.id);text(" title=");text(e.title);
        rect(" outer=",e.layer.outer);rect(" client=",e.layer.client);
        text(" source=");number(e.width);text("x");number(e.height);
        text(" alpha=");number(e.layer.alpha);text(" maximized=");number(e.maximized);
        text(" metricsValid=");number(valid);
        if(i>rainbow&&valid){const auto& m=metrics[i-rainbow-1];
            text(" coverPx=");number(m.cover_pixels);text(" newlyHiddenPx=");number(m.newly_hidden_pixels);
            text(" lowerAlphaClientPx=");number(m.lower_alpha_client_pixels);}
        *end=0;print(line);
    }
}
#endif
// Bounded, coarse samples after layout/alpha changes. Phase times include GPU
// sleeps; these are elapsed milliseconds, not CPU utilization measurements.
static void wm_gpu_perf_sample(uint64_t now) {
    if((!GPU_RCS_ALPHA && !g_gpu_compose_hook)||!g_desktop_ready||!tsc_hz)return;
    static uint64_t signature=0,started=0,frames=0;
    static GpuPerfCounters previous{};
    static unsigned remaining=0,printed=0;
    if(printed>=32)return;
    uint64_t current=wm_gpu_perf_layout_signature();
    if(current!=signature||!started){
        signature=current;started=now;frames=g_compositor_frame;previous=g_gpu_perf;remaining=2;
        if(GPU_RCS_ALPHA){
            for(auto& rejection:g_scanout_rejections)rejection={};
        }
        g_rain_slowest={};g_gpu_timeline_slowest={};g_gpu_execution_slowest={};g_gpu_timeline_last_latch=0;
        if(GPU_RCS_ALPHA)g_gpu_flip_cadence.last_tick=0;
        g_gpu_flip_cadence.reset_sample();
        return;
    }
    if(!remaining||now-started<tsc_hz*2)return;
    auto stats=g_gpu_perf;
#if GPU_WORKLOAD
    if(remaining==2)wm_gpu_geometry_sample(signature);
#endif
    char line[512];char* end=line;
    auto text=[&](const char* value){while(*value&&end<line+sizeof(line)-1)*end++=*value++;};
    auto number=[&](uint64_t value){char digits[24];to_str(value,digits);text(digits);};
    auto ms=[&](uint64_t value){number(value*1000/tsc_hz);};
    text("GPU perf: window=");ms(now-started);text("ms loops=");number(g_compositor_frame-frames);
    text(" bcs=");number(stats.bcs_batches-previous.bcs_batches);
    text(" rcs=");number(stats.rcs_draws-previous.rcs_draws);
    text(" uploadKiB=");number((stats.upload_bytes-previous.upload_bytes)/1024);
    text(" reused=");number(stats.upload_reuses-previous.upload_reuses);
    text(" elapsed-ms scene=");ms(stats.scene_ticks-previous.scene_ticks);
    text(" present=");ms(stats.present_ticks-previous.present_ticks);
    text(" bcs=");ms(stats.bcs_wait-previous.bcs_wait);
    text(" rcs=");ms(stats.rcs_wait-previous.rcs_wait);
    text(" source=");ms(stats.upload_ticks-previous.upload_ticks);
    text(" handoff=");ms(stats.handoff_ticks-previous.handoff_ticks);
    text(" cpuAlpha=");number(stats.cpu_alpha_draws-previous.cpu_alpha_draws);
    text(" alphaKiPx=");number((stats.cpu_alpha_pixels-previous.cpu_alpha_pixels)/1024);
    text(" alphaMs=");ms(stats.cpu_alpha_ticks-previous.cpu_alpha_ticks);
    text(" rcsAlphaKiPx=");number((stats.rcs_alpha_pixels-previous.rcs_alpha_pixels)/1024);
    text(" alphaSavedKiPx=");number((stats.rcs_alpha_skipped-previous.rcs_alpha_skipped)/1024);
    *end=0;print(line);
    // Companion records share the window duration; keep each line bounded.
    end=line;text("GPU present: window=");ms(now-started);text("ms calls=");
    number(stats.present_calls-previous.present_calls);
    text(" elapsed-ms copy=");ms(stats.scanout_copy_ticks-previous.scanout_copy_ticks);
    text(" overlay=");ms(stats.overlay_ticks-previous.overlay_ticks);
    text(" flip=");ms(stats.flip_ticks-previous.flip_ticks);
    const auto cadence=g_gpu_flip_cadence;
    text(" intervals=");number(cadence.intervals);
    text(" avgFlipMs=");ms(cadence.intervals?cadence.total_ticks/cadence.intervals:0);
    text(" maxFlipMs=");ms(cadence.max_ticks);
    text(" skippedRefresh=");number(cadence.intervening_refreshes);
    text(" over2Refresh=");number(cadence.over_two_refreshes);
    text(" cpuCopies=");number(stats.scanout_cpu_copies-previous.scanout_cpu_copies);
    *end=0;print(line);
    end=line;text("GPU sleep: window=");ms(now-started);text("ms");
    auto sleeps=[&](const char* label,const GpuPerfSleep& value,const GpuPerfSleep& old){
        text(label);number(value.calls-old.calls);text("/");ms(value.ticks-old.ticks);
        text("/");number(value.at_least_2ms-old.at_least_2ms);
        text("/");number(value.at_least_10ms-old.at_least_10ms);
    };
    sleeps(" bcs=",stats.bcs_sleep,previous.bcs_sleep);
    sleeps(" rcs=",stats.rcs_sleep,previous.rcs_sleep);
    sleeps(" flip=",stats.flip_sleep,previous.flip_sleep);
    *end=0;print(line);
    if(GPU_RCS_ALPHA){
        end=line;text("GPU frame: layout=");number(signature);
        text(" menu=");number(g_menu_open);
        text(" clipboard=");number(g_clipboard_open);
        text(" taskbarPopup=");number(g_taskbar_popup);
        text(" directFlips=");number(stats.direct_flips-previous.direct_flips);
#if GPU_CURSOR_SAVE
        text(" cursorRestores=");number(stats.cursor_restores-previous.cursor_restores);
#endif
        text(" repairRects=");number(stats.repair_rects-previous.repair_rects);
        text(" repairKiPx=");number((stats.repair_pixels-previous.repair_pixels)/1024);
        text(" seedKiB=");number((stats.scanout_seed_bytes-previous.scanout_seed_bytes)/1024);
        text(" menuMs=");ms(stats.menu_ticks-previous.menu_ticks);
        text(" menuRenders=");number(stats.menu_renders-previous.menu_renders);
        text(" menuReuses=");number(stats.menu_reuses-previous.menu_reuses);
          text(" animationFrames=");number(stats.retained_animation_frames-previous.retained_animation_frames);
          text(" animationKiPx=");number((stats.retained_animation_pixels-previous.retained_animation_pixels)/1024);
          text(" alphaFrames=");number(stats.retained_alpha_frames-previous.retained_alpha_frames);
          text(" shadowRowFrames=");number(stats.shadow_row_frames-previous.shadow_row_frames);
          text(" opaqueRetainedKiPx=");number((stats.opaque_retained_pixels-previous.opaque_retained_pixels)/1024);
          text(" opaqueScans=");number(stats.opaque_scans-previous.opaque_scans);
          text(" opaqueScanUs=");number((stats.opaque_scan_ticks-previous.opaque_scan_ticks)*1000000/tsc_hz);
        *end=0;print(line);
#if GPU_ALPHA_REPAIR_BATCH
          end=line;text("GPU alpha repair: layout=");number(signature);
          text(" frames=");number(stats.alpha_repair_frames-previous.alpha_repair_frames);
          text(" rects=");number(stats.alpha_repair_rects-previous.alpha_repair_rects);
          text(" cancels=");number(stats.alpha_repair_cancels-previous.alpha_repair_cancels);
          *end=0;print(line);
#endif
#if GPU_RETAINED_MENU || GPU_EXACT_REPAIRS
          end=line;text("GPU repair plan: layout=");number(signature);
          text(" cachedMenuRects=");number(stats.menu_cache_repairs-previous.menu_cache_repairs);
          text(" opaqueRepairRects=");number(stats.opaque_repair_rects-previous.opaque_repair_rects);
          text(" exactFrames=");number(stats.exact_repair_frames-previous.exact_repair_frames);
          text(" exactKiPx=");number((stats.exact_repair_pixels-previous.exact_repair_pixels)/1024);
          text(" menuFrames=");number(stats.retained_menu_frames-previous.retained_menu_frames);
          text(" menuKiPx=");number((stats.retained_menu_pixels-previous.retained_menu_pixels)/1024);
          text(" menuCloseDeferred=");number(stats.retained_menu_close_deferrals-previous.retained_menu_close_deferrals);
          text(" sceneBatches=");number(stats.scene_repair_batches-previous.scene_repair_batches);
          text(" sceneRects=");number(stats.scene_repair_rects-previous.scene_repair_rects);
          text(" trayRects=");number(stats.tray_repair_rects-previous.tray_repair_rects);
          text(" trayUs=");number((stats.tray_repair_ticks-previous.tray_repair_ticks)*1000000/tsc_hz);
          text(" rebases=");number(stats.transition_rebases);
          text(" rebaseFailed=");number(stats.transition_rebase_failed);
          *end=0;print(line);
#endif
          const auto& timeline=g_gpu_timeline_slowest;
          if(GPU_RCS_ALPHA){
              end=line;text("GPU alpha wait: layout=");number(signature);
              text(" windows=");number(stats.alpha_tail_windows-previous.alpha_tail_windows);
              text(" pollUs=");number((stats.alpha_tail_ticks-previous.alpha_tail_ticks)*1000000/tsc_hz);
              text(" predictionUs=");number(stats.alpha_tail_prediction_us);
              *end=0;print(line);
          }
          end=line;text("GPU timeline: layout=");number(signature);
          auto duration=[&](const char* label,uint64_t ticks){text(label);number(ticks*1000000/tsc_hz);};
          auto phase=[&](const char* label,uint64_t tick){
              text(label);
              if(tick&&timeline.begin&&tick>=timeline.begin)number((tick-timeline.begin)*1000000/tsc_hz);
              else text("-");
          };
          duration(" intervalUs=",timeline.interval());
          duration(" startGapUs=",timeline.previous_latch&&timeline.begin>=timeline.previous_latch?
                   timeline.begin-timeline.previous_latch:0);
          phase(" composeUs=",timeline.compose);phase(" submitUs=",timeline.first_submit);
          phase(" readyUs=",timeline.last_ready);phase(" armUs=",timeline.arm);
          phase(" latchUs=",timeline.latch);phase(" finishUs=",timeline.finish);
          duration(" cpuUs=",timeline.cpu);duration(" bcsUs=",timeline.bcs);
          duration(" rcsUs=",timeline.rcs);duration(" sleepUs=",timeline.sleep);
          text(" bcsBatches=");number(timeline.bcs_batches);
          text(" rcsDraws=");number(timeline.rcs_draws);
          text(" refreshes=");number(timeline.refreshes);
          text(" flipPolls=");number(timeline.flip_polls);
          duration(" maxFlipSleepUs=",timeline.max_flip_sleep);
          phase(" refreshSeenUs=",timeline.first_refresh);
          phase(" surfaceSeenUs=",timeline.first_live);
          *end=0;print(line);
          const auto& execution=g_gpu_execution_slowest;
#if GPU_FRAME_DIAGNOSTICS
          if(timeline.max_flip_sleep){
              end=line;text("GPU flip wake: layout=");number(signature);
              duration(" maxSleepUs=",timeline.max_flip_sleep);
              duration(" sleepToReadyUs=",timeline.sleep_to_ready);
              duration(" readyToResumeUs=",timeline.ready_to_resume);
              text(" readyLateTicks=");number(timeline.ready_late_ticks);
              text(" resumeLateTicks=");number(timeline.resume_late_ticks);
              text(" readyPid=");if(timeline.sleep_ready_pid<0)text("-");else number(timeline.sleep_ready_pid);
              *end=0;print(line);
          }
          auto frame_diagnostics=[&](const char* sample,const GpuFrameTimeline& trace){
              end=line;text("GPU planning: layout=");number(signature);text(" sample=");text(sample);
              text(" intervalUs=");number(trace.interval()*1000000/tsc_hz);
              const char* labels[]={" prepare="," direct="," merge="," pack="," regions="," repairs=",
                                    " revalidate="," menuBuild="," menuReplay="," fallback="};
              uint64_t invalid=0;
              for(unsigned i=0;i<GpuFrameTimeline::PhaseCount;++i){text(labels[i]);
                  number(trace.planning[i].wall*1000000/tsc_hz);text("/");
                  number(trace.planning[i].cpu*1000000/tsc_hz);invalid+=trace.planning[i].invalid;}
              text(" invalid=");number(invalid);*end=0;print(line);
              end=line;text("GPU phase: layout=");number(signature);text(" sample=");text(sample);
              text(" intervalUs=");number(trace.interval()*1000000/tsc_hz);
              uint64_t span=0;
              auto point=[&](const char* label,const GpuDisplayPhase& p){text(label);number(p.valid);text("/");
                  number(p.frame);text("/");number(p.line());text("/");number(p.live&0xfffff000u);
                  if(p.valid&&p.last-p.first>span)span=p.last-p.first;};
              point(" prev=",trace.display_previous);point(" start=",trace.display_start);
              point(" before=",trace.display_before);point(" after=",trace.display_after);
              point(" observed=",trace.display_latch);
              text(" armWindowUs=");number(trace.display_after.first>=trace.display_before.last?
                  (trace.display_after.first-trace.display_before.last)*1000000/tsc_hz:0);
              text(" maxReadUs=");number(span*1000000/tsc_hz);*end=0;print(line);
          };
          frame_diagnostics("worst",timeline);frame_diagnostics("gpu",execution);
#endif
          auto engine_timing=[&](const char* engine,const GpuExecutionTiming& timing,bool blitter){
              end=line;text("GPU execution: layout=");number(signature);
              text(" engine=");text(engine);text(" samples=");number(timing.samples);
              text(" invalid=");number(timing.invalid);
              text(" frameIntervalUs=");number(execution.interval()*1000000/tsc_hz);
              text(" observedUs=");number(timing.observed_us);
              text(" queueUs=");number(timing.queue_us);text(" setupUs=");number(timing.setup_us);
              text(blitter?" workUs=":" parseUs=");number(timing.parse_us);text(" rtUs=");number(timing.rt_us);
              text(" drainUs=");number(timing.drain_us);text(" tailUs=");number(timing.tail_us);
              *end=0;print(line);
          };
          engine_timing("BCS",execution.bcs_gpu,true);engine_timing("RCS",execution.rcs_gpu,false);
#if GPU_WORKLOAD
          // Work and hardware timings are from the same sampled frame.
          const auto& work=execution.workload;
          end=line;text("GPU workload: layout=");number(signature);
          text(" frameIntervalUs=");number(execution.interval()*1000000/tsc_hz);
          text(" bcsSamples=");number(execution.bcs_gpu.samples);
          text(" rcsSamples=");number(execution.rcs_gpu.samples);
          text(" bcsBatches=");number(execution.bcs_batches);
          const char* kinds[]={" other="," rainbow="," client="," scanout="};
          for(unsigned i=0;i<(unsigned)GpuCopyKind::Count;++i){
              text(kinds[i]);number(work.copies[i].ops);text("/");number(work.copies[i].bytes);
          }
          text(" fill=");number(work.fill_ops);text("/");number(work.fill_bytes);
          text(" fences=");number(work.fences);
          text(" rcsDraws=");number(work.rcs_draws);text(" rcsRects=");number(work.rcs_rects);
          text(" blendPx=");number(work.blend_pixels);text(" opaquePx=");number(work.opaque_pixels);
          *end=0;print(line);
#endif
        const auto& trace=g_rain_slowest;
        end=line;text("GPU Rainbow: policy=");text(g_cpu_alpha_pixel_limit?"SIMD":"RCS");
        text(" layout=");number(signature);
        text(" directFlips=");number(stats.direct_flips-previous.direct_flips);
        text(" intervalUs=");number(trace.interval*1000000/tsc_hz);
        text(" alphaDraws=");number(trace.alpha_draws);
        auto offset=[&](const char* label,uint64_t tick){
            text(label);
            if(tick)number((tick-trace.update)*1000000/tsc_hz);
            else text("-");
        };
        offset(" submitUs=",trace.alpha_submit);offset(" retiredUs=",trace.alpha_done);
        offset(" simdStartUs=",trace.simd_start);offset(" simdDoneUs=",trace.simd_done);
        offset(" copySubmitUs=",trace.copy_submit);offset(" copyDoneUs=",trace.copy_done);
        offset(" armUs=",trace.arm);offset(" latchUs=",trace.latch);
        *end=0;print(line);
        for(const auto& window:trace.windows){
            if(!window.passes)continue;
            end=line;text("GPU window: layout=");number(signature);
            text(" id=");number(window.id);text(" title=");text(window.title);
            text(" alpha=");number(window.alpha);text(" passes=");number(window.passes);
            auto us=[&](const char* label,uint64_t ticks){text(label);number(ticks*1000000/tsc_hz);};
            us(" startUs=",window.start-trace.update);us(" endUs=",window.end-trace.update);
            us(" shadowUs=",window.shadow);us(" chromeUs=",window.chrome);
            us(" gpuUs=",window.gpu);us(" clientUs=",window.client);
            us(" bcsUs=",window.bcs);us(" rcsUs=",window.rcs);
            *end=0;print(line);
        }
        auto signed_number=[&](int64_t value){
            if(value<0)text("-");
            number(value<0?0-(uint64_t)value:(uint64_t)value);
        };
        for(unsigned i=0;i<scanout_check_count;++i){
            auto& rejection=g_scanout_rejections[i];
            if(!rejection.count)continue;
            end=line;text("GPU direct: layout=");number(signature);
            text(" policy=");text(g_cpu_alpha_pixel_limit?"SIMD":"RCS");
            text(" blockedBy=");text(scanout_check_names[i]);
            text(" attempts=");number(rejection.count);
            text(" lastActual=");signed_number(rejection.actual);
            text(" expected=");signed_number(rejection.expected);
            *end=0;print(line);
            rejection={};
        }
        g_rain_slowest={};
        // Exclude the interval crossing a log write from the next sample.
        g_gpu_timeline_slowest={};
        g_gpu_execution_slowest={};
        g_gpu_timeline_last_latch=0;
        g_gpu_flip_cadence.last_tick=0;
    }
    --remaining;++printed;started=rdtsc_val();frames=g_compositor_frame;previous=stats;
    g_gpu_flip_cadence.reset_sample();
}
static void gpu_direct_end();
static bool gpu_direct_active();
static inline bool wm_compose_dirty_band(uint32_t min_x, uint32_t max_x,
                                     uint32_t min_y, uint32_t max_y, bool do_overlay = true,
                                     bool present = true,const VisibleRegion* repairs=nullptr) {
    bool skip_gpu_scene=false,first_attempt=true;
retry_scene:
    if(!first_attempt){
        gpu_direct_end();repairs=nullptr;
#if GPU_ALPHA_REPAIR_BATCH
        // The full CPU repaint below replaces even repairs deferred by earlier
        // calls. Replaying those clips later would blend their alpha twice.
        wm_cancel_alpha_repairs();
#endif
    }
    first_attempt=false;
    if (!fb || !fb->address || !g_master_backbuffer) return false;
    if (min_x>max_x || min_y>max_y || min_x>=fb->width || min_y>=fb->height) return false;
    if (max_x >= fb->width)  max_x = fb->width - 1;
    if (max_y >= fb->height) max_y = fb->height - 1;
    if (min_y > max_y || min_x > max_x) return false;
    VisibleRect menu_cover{};
    if(g_menu_open){
        int x,y,w,h;wm_menu_geom(&x,&y,&w,&h);
        menu_cover={x,y,x+w,y+h};
    }
    ++g_scene_compose_count;g_scene_compose_pixels+=(uint64_t)(max_x-min_x+1)*(max_y-min_y+1);
    ResourceRead resource_read;

    // Chrome/taskbar currently repaint whole horizontal spans on damaged rows.
    // Restore text across those same spans, clipping vertically to avoid blending
    // antialiasing twice onto rows whose backgrounds were not recomposed.
    g_ui_clip_left = 0; g_ui_clip_top = min_y;
    g_ui_clip_right = fb->width; g_ui_clip_bottom = max_y + 1;
    // Only the compositor calls this function. Keep frame snapshots off its
    // stack; nested visibility, GPU waits and timer interrupts need headroom.
    static WinRect rects[MAX_WINDOWS];static Window* frame_windows[MAX_WINDOWS];
    static int frame_order[MAX_WINDOWS];int frame_count;
    struct Source { uint32_t* pixels;int w,h;uint32_t pitch;bool alpha;uint64_t revision; };
    static Source sources[MAX_WINDOWS];
    bool alpha_scene=false;
    {TtyIrqGuard guard;frame_count=g_window_count;
        for(int i=0;i<frame_count;++i){auto* win=g_window_list[i];frame_windows[i]=win;frame_order[i]=g_window_order[i];
            wm_get_win_rect(win,&rects[i]);sources[i]={win->buffer,win->canvas_width,win->canvas_height,win->buffer_pitch,win->premultiplied};
            if(rects[i].visible&&win->premultiplied)alpha_scene=true;
            if(rects[i].visible)gpu_launch_sample(win->id,g_compositor_frame);
            const auto& slot=win->published[win->published_slot];
            if(slot.pixels && !g_gpu_quarantine_sources)sources[i]={slot.pixels,slot.width,slot.height,(uint32_t)slot.width*4,win->premultiplied,slot.revision};}}
    // Ignore shadow areas hidden by higher opaque windows. This preserves the
    // fullscreen fast paths and retained client updates away from shadow edges.
    auto exposed_shadow=[&](VisibleRect damage){
        for(int s=0;s<frame_count;++s){
            int i=frame_order[s];const auto& r=rects[i];auto* win=frame_windows[i];
            if(!r.visible||!win->shadow_enabled||win->maximized)continue;
            VisibleRect outer{r.x1,r.y1,r.x2,r.y2};
            auto hit=visibility_intersect(window_shadow_bounds(outer),damage);
            if(!visibility_valid(hit))continue;
            VisibleRegion region{};region.count=1;region.rects[0]=hit;
            if(!visibility_subtract(&region,outer))return true;
            for(int t=s+1;t<frame_count&&region.count;++t){
                int j=frame_order[t];if(!rects[j].visible)continue;
                auto cover=VisibleRect{rects[j].x1,rects[j].y1,rects[j].x2,rects[j].y2};
                if(sources[j].alpha)cover.y2=cover.y1+(int)g_title_h;
                if(!visibility_subtract(&region,cover))return true;
            }
            if(region.count)return true;
        }
        return false;
    };
    bool shadow_damage=exposed_shadow({(int)min_x,(int)min_y,(int)max_x+1,(int)max_y+1});
    bool show_desktop_stats=true;
    for(int i=0;i<frame_count;++i)
        if(rects[i].visible&&frame_windows[i]->maximized){show_desktop_stats=false;break;}

    // Only this thread owns the plan; keep its bounded storage off the kernel
    // stack. Match the existing full-width scene updates on damaged rows.
    static GpuScenePlan gpu_plan{};
    static GpuScenePlan render_plan{};
    static VisibleRegion retained_background{};
    render_plan.count=0;render_plan.accelerated=0;
    gpu_plan.client_count=frame_count;gpu_plan.scroll=rain_scroll;
    gpu_plan.rainbow_colors=g_rainbow_wide;
    gpu_plan.count=0;gpu_plan.accelerated=0;
    bool fullscreen_fast=false;
    bool scaled_fast=false;
    bool retained_fast=false;
    bool direct=gpu_direct_active();
    if(g_gpu_compose_hook && present && !skip_gpu_scene && !direct && !alpha_scene && !shadow_damage){
        for(int s=0;s<frame_count;++s){
            int index=frame_order[s];const auto& r=rects[index];auto* win=frame_windows[index];
            const auto& source=sources[index];
            gpu_plan.clients[s]={{r.x1,r.y1,r.x2,r.y2},{r.x1,r.y1+(int)g_title_h,r.x2,r.y2},
                source.pixels,(uint32_t)source.w,(uint32_t)source.h,source.pitch,
                win&&r.visible,win&&win->is_system&&win->type==WIN_TYPE_RAIN,win&&!win->maximized,source.alpha,source.revision};
        }
        if(!g_menu_open && !g_clipboard_open && !g_taskbar_popup)
            fullscreen_fast=gpu_rain_damage_plan(&gpu_plan,{(int)min_x,(int)min_y,(int)max_x+1,(int)max_y+1});
        if(!fullscreen_fast && g_gpu_render_hook){
            render_plan.client_count=frame_count;
            for(unsigned s=0;s<(unsigned)frame_count;++s)render_plan.clients[s]=gpu_plan.clients[s];
            if(!g_menu_open && !g_clipboard_open && !g_taskbar_popup)
                scaled_fast=gpu_scaled_damage_plan(&render_plan,
                    {(int)min_x,(int)min_y,(int)max_x+1,(int)max_y+1});
        }
        if(!fullscreen_fast && !scaled_fast && !g_menu_open && !g_clipboard_open && !g_taskbar_popup &&
           max_y<fb->height-g_taskbar_h && (!show_desktop_stats||min_y>=60)){
            VisibleRect damage{(int)min_x,(int)min_y,(int)max_x+1,(int)max_y+1};
            gpu_scene_plan(&gpu_plan,damage,true);
            render_plan.client_count=frame_count;
            for(unsigned s=0;s<(unsigned)frame_count;++s)render_plan.clients[s]=gpu_plan.clients[s];
            if(g_gpu_render_hook)gpu_scaled_scene_plan(&render_plan,damage,true);
            retained_fast=gpu_retained_damage(gpu_plan,render_plan,damage,&retained_background);
        }
        if(!fullscreen_fast && !scaled_fast && !retained_fast)gpu_scene_plan(&gpu_plan,{0,(int)min_y,(int)fb->width,
            (int)(max_y+1 < fb->height-g_taskbar_h ? max_y+1 : fb->height-g_taskbar_h)});
        if(!fullscreen_fast && !scaled_fast && !retained_fast && g_gpu_render_hook){
            gpu_scaled_scene_plan(&render_plan,{0,(int)min_y,(int)fb->width,
                (int)(max_y+1<fb->height-g_taskbar_h?max_y+1:fb->height-g_taskbar_h)});
            gpu_plan.accelerated|=render_plan.accelerated;
        }
    }
    bool ordered_scene=direct||alpha_scene||(!fullscreen_fast&&!scaled_fast&&!retained_fast&&
        exposed_shadow({0,(int)min_y,(int)fb->width,(int)max_y+1}));
    bool bounded_scene=ordered_scene&&(!g_menu_open||wm_retained_menu_ready())&&!g_clipboard_open&&!g_taskbar_popup&&
        (min_y>=60||(GPU_EXACT_REPAIRS&&direct&&!show_desktop_stats))&&max_y<fb->height-g_taskbar_h;
    if(ordered_scene){
        // Shell/HUD paths still paint full rows. Ordinary client updates clip
        // background, cached chrome, shadows and clients to the real damage.
        if(!bounded_scene){min_x=0;max_x=fb->width-1;}
        g_ui_clip_left=min_x;g_ui_clip_right=max_x+1;
        gpu_plan.count=0;gpu_plan.accelerated=0;
        render_plan.count=0;render_plan.accelerated=0;
    }
    
    // Metrics reset (B/W/r/L are updated DURING the frame, V is from last)
    g_time_bg_ticks   = 0;
    g_time_win_ticks  = 0;
    g_time_rain_ticks = 0;
    g_time_lock_ticks = 0;
    g_time_vram_ticks = 0;

    uint64_t t0 = rdtsc_val();
    uint64_t t1, t2, t3;

    if(retained_fast){
        // Only uncovered desktop gaps need CPU pixels. Window chrome and
        // unchanged clients remain in the master; GPU copies cover all other
        // damage, including the backgrounds beneath resize-grip decorations.
        for(unsigned i=0;i<retained_background.count;++i){
            auto r=retained_background.rects[i];
            for(int y=r.y1;y<r.y2;++y){
                auto* row=(uint32_t*)(g_master_backbuffer+(uint64_t)y*fb->pitch)+r.x1;
                if(g_wallpaper)memcpy_vram_sse_headless(row,
                    g_wallpaper+(uint64_t)y*fb->width+r.x1,(r.x2-r.x1)*4);
                else memset_32(row,0xFF008080,r.x2-r.x1);
            }
        }
        t1=rdtsc_val();g_time_bg_ticks=t1-t0;
        if(gpu_plan.count&&!g_gpu_compose_hook(gpu_plan)){
            skip_gpu_scene=true;g_rain_surface_current=false;
            min_x=0;min_y=0;max_x=fb->width-1;max_y=fb->height-1;goto retry_scene;
        }
        if(render_plan.count&&(!g_gpu_render_hook||!g_gpu_render_hook(render_plan))){
            g_gpu_render_hook=nullptr;
            min_x=0;min_y=0;max_x=fb->width-1;max_y=fb->height-1;goto retry_scene;
        }
        for(unsigned i=0;i<gpu_plan.client_count;++i){
            const auto& c=gpu_plan.clients[i];
            if(!c.visible||!c.resize_grip)continue;
            for(int bottom=1;bottom<12;++bottom)for(int right=1;right<12;++right){
                if(right+bottom!=6&&right+bottom!=10&&right+bottom!=14)continue;
                int x=c.client.x2-1-right,y=c.client.y2-1-bottom;
                if(x<(int)min_x||x>(int)max_x||y<(int)min_y||y>(int)max_y||
                   x<c.client.x1||y<c.client.y1||!gpu_scene_pixel_exposed(gpu_plan,i,x,y))continue;
                ((uint32_t*)(g_master_backbuffer+(uint64_t)y*fb->pitch))[x]=0xff81728f;
            }
        }
        ++g_gpu_retained_fast_count;
        t2=rdtsc_val();g_time_win_ticks=t2-t1;
    }else if(fullscreen_fast || scaled_fast){
        bool complete=scaled_fast?g_gpu_render_hook(render_plan):g_gpu_compose_hook(gpu_plan);
        if(!complete){
            if(scaled_fast)g_gpu_render_hook=nullptr;
            else {skip_gpu_scene=true;g_rain_surface_current=false;}
            min_x=0;min_y=0;max_x=fb->width-1;max_y=fb->height-1;goto retry_scene;
        }
        if(scaled_fast)++g_gpu_scaled_fast_count;else ++g_gpu_fullscreen_fast_count;
        t1=t0;t2=rdtsc_val();g_time_win_ticks=t2-t0;
    }else{

    if(ordered_scene)++g_gpu_ordered_scene_count;

    // Repairs share one snapshot and traverse the stack once. Keep exact clips
    // for CPU pixels too: drawing bounding-box holes would accumulate alpha.
    const VisibleRect scene_bounds{(int)min_x,(int)min_y,(int)max_x+1,(int)max_y+1};
    auto select_repair=[&](unsigned i){
        auto r=repairs?repairs->rects[i]:scene_bounds;
        min_x=r.x1;max_x=r.x2-1;min_y=r.y1;max_y=r.y2-1;
        g_ui_clip_left=bounded_scene?min_x:0;g_ui_clip_right=bounded_scene?max_x+1:fb->width;
        g_ui_clip_top=min_y;g_ui_clip_bottom=max_y+1;
    };
    unsigned repair_count=repairs?repairs->count:1;
    // Stage 1: Desktop wallpaper (hole-punched)
    for(unsigned repair=0;repair<repair_count;++repair){
    select_repair(repair);
    for (uint32_t y = min_y; y <= max_y; y++) {
        uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)y * fb->pitch);
        uint32_t* wrow = nullptr;
        if (g_wallpaper && y < fb->height) {
            wrow = g_wallpaper + (uint64_t)y * fb->width;
        }
        if (y >= fb->height - g_taskbar_h) continue; // Taskbar area

        // Collect opaque spans for this scanline
        struct Span { int32_t x1, x2; };
        Span spans[MAX_WINDOWS+1];
        int num_spans = 0;
        
        for (int i = 0; i < frame_count; i++) {
            if (frame_windows[i] && rects[i].visible && (int32_t)y >= rects[i].y1 && (int32_t)y < rects[i].y2 &&
                (!sources[i].alpha || (int32_t)y<rects[i].y1+(int32_t)g_title_h)) {
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
        if((int)y>=menu_cover.y1 && (int)y<menu_cover.y2){
            int left=menu_cover.x1>(int)min_x?menu_cover.x1:min_x;
            int right=menu_cover.x2<(int)max_x+1?menu_cover.x2:max_x+1;
            if(left<right)spans[num_spans++]={left,right};
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
    
    }
    min_x=scene_bounds.x1;max_x=scene_bounds.x2-1;min_y=scene_bounds.y1;max_y=scene_bounds.y2-1;
    t1 = rdtsc_val();
    g_time_bg_ticks = t1 - t0;

    // Stage 2: Windows in stack order
    int first_window=0;
    if(direct){
        // Any opaque client covering this damage makes lower layers hidden.
        for(int s=0;s<frame_count;++s){
            int i=frame_order[s];const auto& r=rects[i];
            if(r.visible && sources[i].pixels &&
               !sources[i].alpha && r.x1<=(int)min_x && r.x2>(int)max_x &&
               r.y1+(int)g_title_h<=(int)min_y && r.y2>(int)max_y)first_window=s;
        }
    }
    for (int s = first_window; s < frame_count; s++) {
        int idx = frame_order[s];
        Window* win = frame_windows[idx];
        if (!win || !rects[idx].visible) continue;
        RainbowWindowTimer window_timer(idx,win->id,win->title,sources[idx].alpha);
        static GpuScenePlan layer_batch{};
        layer_batch.count=0;layer_batch.accelerated=0;
        unsigned layer_engine=0;
        for(unsigned repair=0;repair<repair_count;++repair){
        select_repair(repair);
        WinRect r = rects[idx];
        Source source_surface=sources[idx];int display_w=r.x2-r.x1,display_h=r.y2-r.y1-g_title_h;

        if(win->shadow_enabled&&!win->maximized){
            VisibleRect covers[MAX_WINDOWS+1];unsigned cover_count=0;
            if(visibility_valid(menu_cover))covers[cover_count++]=menu_cover;
            for(int t=s+1;t<frame_count;++t){
                int higher=frame_order[t];if(!rects[higher].visible)continue;
                const auto& cover=rects[higher];
                covers[cover_count++]={cover.x1,cover.y1,cover.x2,
                    sources[higher].alpha?cover.y1+(int)g_title_h:cover.y2};
            }
            window_shadow_draw({g_master_backbuffer,(int)fb->width,(int)(fb->height-g_taskbar_h),(int)fb->pitch},
                {r.x1,r.y1,r.x2,r.y2},{(int)min_x,(int)min_y,(int)max_x+1,(int)max_y+1},covers,cover_count);
        }

        int32_t dr1 = (r.y1 > (int32_t)min_y) ? r.y1 : (int32_t)min_y;
        // Window bounds and row loops are half-open; damage max_y is inclusive.
        // Omitting its last row leaves lower-window chrome in the saved scene,
        // later exposed by cursor restoration across a small caret repaint.
        int32_t bottom = (int32_t)max_y + 1;
        int32_t dr2 = (r.y2 < bottom) ? r.y2 : bottom;
        // Don't draw windows over taskbar area
        if (dr2 > (int32_t)(fb->height - g_taskbar_h)) dr2 = fb->height - g_taskbar_h;
        if (dr1 >= dr2) continue;

        // Unified surface compositor: every window (system or user) is backed by its surface buffer
        window_timer.next(&RainbowWindowTrace::chrome);
        wm_draw_window_chrome(win,r.x1,r.y1,r.x2-r.x1,min_y,max_y,wm_window_icons(win).title);
        window_timer.next(&RainbowWindowTrace::gpu);
        if (source_surface.pixels) {
            bool accelerated=(gpu_plan.accelerated & (uint32_t(1)<<s))!=0;
            bool ordered_accelerated=false;
            if(ordered_scene && present && !skip_gpu_scene &&
               !(source_surface.alpha&&alpha_cpu_eligible(
                   {r.x1,r.y1+(int)g_title_h,r.x2,r.y2},
                   {(int)min_x,dr1,(int)max_x+1,dr2},g_cpu_alpha_pixel_limit))){
                static GpuScenePlan ordered_plan{};
                GpuSceneClient client{{r.x1,r.y1,r.x2,r.y2},
                    {r.x1,r.y1+(int)g_title_h,r.x2,r.y2},source_surface.pixels,
                    (uint32_t)source_surface.w,(uint32_t)source_surface.h,source_surface.pitch,
                    true,win->is_system&&win->type==WIN_TYPE_RAIN,!win->maximized,source_surface.alpha,source_surface.revision};
                ordered_plan.scroll=rain_scroll;ordered_plan.rainbow_colors=g_rainbow_wide;
                unsigned engine=gpu_ordered_scene_plan(&ordered_plan,client,{(int)min_x,(int)min_y,(int)max_x+1,dr2});
                VisibleRect opaque[MAX_WINDOWS+1];unsigned opaque_count=0;
                if(visibility_valid(menu_cover))opaque[opaque_count++]=menu_cover;
                for(int t=s+1;t<frame_count;++t){
                    int higher=frame_order[t];if(!rects[higher].visible)continue;
                    const auto& cover=rects[higher];
                    opaque[opaque_count++]={cover.x1,cover.y1,cover.x2,
                        sources[higher].alpha?cover.y1+(int)g_title_h:cover.y2};
                }
                gpu_ordered_occlude(&ordered_plan,opaque,opaque_count);
                auto hook=engine==1?g_gpu_compose_hook:engine==2?g_gpu_render_hook:nullptr;
                if(hook){
                    bool deferred=false;
#if GPU_ALPHA_REPAIR_BATCH
                    if(engine==2&&bounded_scene&&!do_overlay&&gpu_direct_active()&&win->id==g_deferred_alpha_id){
                        bool topmost=true;
                        for(int t=s+1;t<frame_count;++t)if(rects[frame_order[t]].visible){topmost=false;break;}
                        if(topmost)deferred=g_deferred_alpha.append(ordered_plan,g_retained_animation_plan);
                    }
#endif
                    if(repairs&&!deferred){
                        if((layer_engine&&layer_engine!=engine)||!gpu_scene_append_clips(&layer_batch,ordered_plan)){
                            skip_gpu_scene=true;g_rain_surface_current=false;
                            min_x=0;min_y=0;max_x=fb->width-1;max_y=fb->height-1;goto retry_scene;
                        }
                        layer_engine=engine;
                    }
                    if(!repairs&&!deferred&&ordered_plan.count&&!hook(ordered_plan)){
                        if(engine==2)g_gpu_render_hook=nullptr;
                        skip_gpu_scene=true;g_rain_surface_current=false;
                        min_x=0;min_y=0;max_x=fb->width-1;max_y=fb->height-1;goto retry_scene;
                    }
                    ordered_accelerated=true;
                }
            }
            window_timer.next(&RainbowWindowTrace::client);
            bool procedural_rain=win->is_system&&win->type==WIN_TYPE_RAIN&&!g_rain_surface_current;
            if(procedural_rain && (source_surface.w!=display_w||source_surface.h!=display_h)){
                rainbow_render_surface(source_surface.pixels,source_surface.w,source_surface.h,
                                       source_surface.pitch,rain_scroll,g_rainbow_wide);
                procedural_rain=false;g_rain_surface_current=true;
            }
            if(source_surface.alpha){
                int alpha_top=dr1;
                if(ordered_accelerated&&alpha_top<r.y2-(win->maximized?0:12))
                    alpha_top=r.y2-(win->maximized?0:12);
                GpuPerfTimer timer(g_gpu_perf.cpu_alpha_ticks);
                uint64_t pixels=alpha_render(
                    {(uint32_t*)g_master_backbuffer,(int)fb->width,(int)fb->height,(int)fb->pitch},
                    {source_surface.pixels,source_surface.w,source_surface.h,(int)source_surface.pitch},
                    {r.x1,r.y1+(int)g_title_h,r.x2,r.y2},
                    {(int)min_x,alpha_top,(int)max_x+1,dr2});
                if(pixels){
                    ++g_gpu_perf.cpu_alpha_draws;g_gpu_perf.cpu_alpha_pixels+=pixels;
                    if(g_rain_trace.update){
                        if(!g_rain_trace.simd_start)g_rain_trace.simd_start=timer.started;
                        g_rain_trace.simd_done=rdtsc_val();
                    }
                }
            }
            for (int32_t y = dr1; y < dr2; y++) {
                if (y < r.y1 + (int32_t)g_title_h) continue;
                if (accelerated && y < r.y2-(gpu_plan.clients[s].resize_grip?12:0)) continue;
                if (ordered_accelerated && y < r.y2-(win->maximized?0:12)) continue;
                uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)y * fb->pitch);
                int32_t ry = y - (r.y1 + (int32_t)g_title_h);
                int32_t wx = r.x1; if (wx < 0) wx = 0;
                int32_t ww = (r.x2 - r.x1); if (r.x1 < 0) ww += r.x1;
                if (wx + ww > (int32_t)fb->width) ww = (int32_t)fb->width - wx;
                if(bounded_scene){
                    int right=wx+ww;if(right>(int)max_x+1)right=max_x+1;
                    if(wx<(int)min_x)wx=min_x;
                    ww=right-wx;
                }
                if (ww > 0) {
                    uint32_t src_off = (uint32_t)(wx-r.x1);
                    if(source_surface.alpha){
                        // Optimized clipped blend above; retain the CPU grip.
                    }else if(procedural_rain)
                        wm_fill_rainbow_row(drow+wx,ww,rain_scroll,src_off);
                    else if(source_surface.w==display_w&&source_surface.h==display_h)
                        memcpy_vram_sse_headless(drow + wx, (uint32_t*)((uint8_t*)source_surface.pixels + (uint64_t)ry * source_surface.pitch) + src_off, ww * 4);
                    else {
                        uint32_t sy=(uint64_t)ry*source_surface.h/display_h;
                        auto* source=(uint32_t*)((uint8_t*)source_surface.pixels+(uint64_t)sy*source_surface.pitch);
                        for(int32_t px=0;px<ww;++px)drow[wx+px]=source[(uint64_t)(src_off+px)*source_surface.w/display_w];
                    }
                    int from_bottom=r.y2-1-y;
                    if((accelerated?gpu_plan.clients[s].resize_grip:!win->maximized)&&from_bottom>=1&&from_bottom<12)for(int right=1;right<12;++right)
                        if(right+from_bottom==6||right+from_bottom==10||right+from_bottom==14){int px=r.x2-1-right;
                            if(px>=wx&&px<wx+ww)drow[px]=0xff81728f;}
                }
            }
        }
        } // repair clips for this window
        if(repairs&&layer_batch.count){
            window_timer.next(&RainbowWindowTrace::gpu);
            auto hook=layer_engine==1?g_gpu_compose_hook:g_gpu_render_hook;
            if(!hook||!hook(layer_batch)){
                if(layer_engine==2)g_gpu_render_hook=nullptr;
                skip_gpu_scene=true;g_rain_surface_current=false;
                min_x=0;min_y=0;max_x=fb->width-1;max_y=fb->height-1;goto retry_scene;
            }
        }
    }
    min_x=scene_bounds.x1;max_x=scene_bounds.x2-1;min_y=scene_bounds.y1;max_y=scene_bounds.y2-1;
    g_ui_clip_left=bounded_scene?min_x:0;g_ui_clip_right=bounded_scene?max_x+1:fb->width;
    g_ui_clip_top=min_y;g_ui_clip_bottom=max_y+1;

    // Complete GPU writes before the CPU draws menus/HUD/cursor or reads the
    // master buffer. On a failed batch, recompute the whole scene through the
    // CPU path; the driver has disabled submission and quiesced or quarantined
    // its old destination so late DMA cannot corrupt the replacement buffer.
    if(gpu_plan.count && !g_gpu_compose_hook(gpu_plan)){
        skip_gpu_scene=true;
        g_rain_surface_current=false;
        min_x=0;min_y=0;max_x=fb->width-1;max_y=fb->height-1;
        // Reuse this frame instead of recursively consuming a second large
        // compositor stack frame. The backward jump releases ResourceRead.
        goto retry_scene;
    }

    // ── Shared shell control: taskbar and Start menu ─────────────────────────────
    if(render_plan.count && (!g_gpu_render_hook || !g_gpu_render_hook(render_plan))){
        g_gpu_render_hook=nullptr;
        min_x=0;min_y=0;max_x=fb->width-1;max_y=fb->height-1;
        goto retry_scene;
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

        // ── Start button ─────────────────────────────────────────────────────
        const int32_t PAD = 5;
        int32_t sb_x = 6,   sb_y = tb_y + PAD;
        int32_t sb_h = (int32_t)g_taskbar_h - PAD * 2;

        wm_draw_button(min_y,max_y,sb_x, sb_y, g_sb_w, sb_h,
                0xFF4a7ad8, 0xFF2a50b0,   // blue gradient
                0xFF1a3a90,               // dark blue border
                true);

        // "# START" label, centered
        const char* start_lbl   = "# START";
        uint32_t char_h = wm_text_height(g_font_scale_100);
        wm_draw_string(sb_x + ((int32_t)g_sb_w - wm_text_width(start_lbl, g_font_scale_100)) / 2,
                    sb_y + (int32_t)(sb_h - char_h) / 2,
                    start_lbl, 0xFFFFFFFF, g_font_scale_100);
        
        wm_draw_taskbar_tray({0,tbr1,(int)fb->width,tbr2+1},false);

        // ── Window task buttons ───────────────────────────────────────────────
        const int32_t WBTN_GAP = (int32_t)(4 * fb->height) / 1080;
        int32_t cur_x = sb_x + g_sb_w + (int32_t)(10 * fb->height) / 1080;
        auto tasks=wm_taskbar_snapshot();
        int32_t button_width=tasks.layout.width;

        for (int group = 0; group < tasks.layout.shown; group++) {
            Window* win = tasks.windows[tasks.layout.groups[group].representative];
            bool active = tasks.layout.groups[group].active;

            uint32_t top_c  = active ? 0xFF3d4d80 : 0xFF23283d;
            uint32_t bot_c  = active ? 0xFF28366a : 0xFF181c30;
            uint32_t edge_c = active ? 0xFF5568b0 : 0xFF2c3258;

            wm_draw_button(min_y,max_y,cur_x, sb_y, button_width, sb_h, top_c, bot_c, edge_c, active);

            if(win->minimized){
                int py=sb_y+sb_h-4;
                if(py>=(int)min_y&&py<=(int)max_y&&py>=0&&py<(int)fb->height){
                    auto* pixels=(uint32_t*)(g_master_backbuffer+(uint64_t)py*fb->pitch);
                    for(int px=cur_x+3;px<cur_x+button_width-3;++px)
                        if(px>=0&&px<(int)fb->width)pixels[px]=0xFF9C87BB;
                }
            }

            if (active) {
                int32_t bar_y = sb_y;
                if (bar_y >= (int32_t)min_y && bar_y <= (int32_t)max_y &&
                    bar_y >= 0 && (uint32_t)bar_y < fb->height) {
                    uint32_t* drow = (uint32_t*)(g_master_backbuffer + (uint64_t)bar_y * fb->pitch);
                    int32_t bar_x1 = cur_x + 2, bar_x2 = cur_x + button_width - 3;
                    if (bar_x1 < 0) bar_x1 = 0;
                    if (bar_x2 >= (int32_t)fb->width) bar_x2 = (int32_t)fb->width - 1;
                    if (bar_x2 > bar_x1) memset_32(drow + bar_x1, 0xFF6688FF, (uint32_t)(bar_x2 - bar_x1));
                }
            }

            const Image& task_icon=wm_window_icons(win).task;
            int32_t text_offset=8;
            if (task_icon.pixels) {
                wm_draw_icon(task_icon,cur_x+6,sb_y+((int32_t)sb_h-(int32_t)task_icon.height)/2,
                             cur_x,min_y,cur_x+button_width,max_y+1);
                text_offset=12+task_icon.width;
            }
            char label[96];resources::copy(label,sizeof(label),win->title);
            if(tasks.layout.groups[group].count>1){
                char number[24];to_str(tasks.layout.groups[group].count,number);
                resources::append(label,sizeof(label)," (");resources::append(label,sizeof(label),number);resources::append(label,sizeof(label),")");
            }
            if(button_width>text_offset+8)wm_draw_string(cur_x + text_offset, sb_y + (int32_t)(sb_h - char_h) / 2,
                        label,
                        active ? 0xFFEEEEFF : 0xFF8888AA,
                        g_font_scale_100, button_width - text_offset - 8);

            cur_x += button_width + WBTN_GAP;
        }
        if(tasks.layout.overflow_width){
            int x=tasks.layout.overflow_x,w=tasks.layout.overflow_width;
            wm_draw_button(min_y,max_y,x,sb_y,w,sb_h,0xff303955,0xff1c243c,0xff465475,g_taskbar_popup==1);
            int dot=sb_h>=40?3:2;
            int spacing=dot*2;
            int center_x=x+w/2,center_y=sb_y+sb_h/2;
            for(int i=-1;i<=1;++i)for(int y=center_y-dot/2;y<center_y-dot/2+dot;++y){
                if(y<(int)min_y||y>(int)max_y)continue;
                auto* pixels=(uint32_t*)(g_master_backbuffer+(uint64_t)y*fb->pitch);
                for(int dx=0;dx<dot;++dx){
                    int px=center_x+i*spacing-dot/2+dx;
                    if(px>=x&&px<x+w&&px>=0&&px<(int)fb->width)pixels[px]=0xffeeeeff;
                }
            }
        }
    }

    // Keep diagnostic overlays off maximized applications and their controls.
    // Stage 2.6: RAM display (top-right)
    if(show_desktop_stats){
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
    if(show_desktop_stats){
        static char cpu_msg[HUD_MSG_CAP];
        static uint32_t cpu_color;
        if(g_hud_sample_due){
        g_hud_sample_due=false;
        // Keep the same text and alignment across partial redraws until the
        // compositor damages the complete HUD for its next sample.
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
        if(g_gpu_flip_cadence.last_interval && d-cpu_msg < HUD_MSG_CAP-32){
            p = "Flip:"; while(*p) *d++ = *p++;
            d = hud_put_ms1(d,g_gpu_flip_cadence.last_interval);
            p = "ms | "; while(*p) *d++ = *p++;
        }
        p = g_gpu_status; while(*p && (d - cpu_msg) < (HUD_MSG_CAP - 2)) *d++ = *p++;
        *d = '\0';

        cpu_color = g_cpu_usage_pct < 50 ? 0xFF88FF88 :
                    g_cpu_usage_pct < 80 ? 0xFFFFDD44 : 0xFFFF6644;
        }
        uint32_t msg_len = strlen(cpu_msg);
        uint32_t rx = fb->width > msg_len*8+20 ? fb->width-msg_len*8-20 : 0;
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
            wm_draw_mono_string(rx, ry, cpu_msg, cpu_color);
        }
    }
    
    wm_taskbar_render_popup(min_y,max_y);
    wm_draw_start_menu(min_y,max_y);
    if(g_clipboard_open){
        auto panel=wm_clipboard_panel();
        int x=panel.x,y=panel.y,w=panel.w;
        if((int)min_x<x+w&&(int)max_x>=x&&(int)min_y<y+panel.h&&(int)max_y>=y){
            ClipboardPreview previews[CLIPBOARD_VISIBLE_ROWS];
            int count=0,total,first,selected;
            ClipboardHit hover;
            bool can_pin;
            int card_x=x+CLIPBOARD_PAD,card_w=panel.card_right()-card_x;
            int preview_width=panel.action_left()-card_x-18;
            const UiFont* font=wm_ui_font();
            {
                TtyIrqGuard guard;
                total=g_text_clipboard.history_size();first=g_clipboard_first;
                selected=g_clipboard_selected-first;
                hover=wm_clipboard_hit(g_mouse_x,g_mouse_y);
                can_pin=g_text_clipboard.pinned_count()<TextClipboard::pin_limit;
                for(int i=first;i<total&&count<panel.rows;++i){
                    wm_clipboard_preview(g_text_clipboard.history(i),previews[count++],font,preview_width);
                }
            }
            wm_draw_button(min_y,max_y,x,y,w,panel.h,0xff23283d,0xff181c30,0xff303560,false);
            wm_draw_button(min_y,max_y,x,y,w,CLIPBOARD_HEADER,0xff3d4d80,0xff28366a,0xff5568b0,false);
            int line=font->height;
            wm_draw_string(x+12,y+(CLIPBOARD_HEADER-line)/2,"Clipboard history",0xffeeeeff,100,panel.clear_left()-x-20);
            wm_draw_button(min_y,max_y,panel.clear_left(),y+6,CLIPBOARD_CLEAR_WIDTH,CLIPBOARD_HEADER-12,0xff263456,0xff202742,0xff6777a2,false);
            const char* clear="Clear unpinned";
            wm_draw_string(panel.clear_left()+(CLIPBOARD_CLEAR_WIDTH-wm_text_width(clear))/2,
                           y+(CLIPBOARD_HEADER-line)/2,clear,0xffeeeeff,100,CLIPBOARD_CLEAR_WIDTH-8);
            int card_h=CLIPBOARD_ROW-CLIPBOARD_GAP;
            const char* hint="Select an entry to paste";
            if(!count)wm_draw_string(card_x+10,panel.list_top()+(card_h-line)/2,"Nothing copied yet",0xffb9c5ee,100,card_w-20);
            for(int i=0;i<count;++i){
                int top=panel.list_top()+i*CLIPBOARD_ROW;
                bool active=i==selected,pointed=first+i==hover.index;
                uint32_t fill=active?0xff344469:pointed?0xff2d3a55:0xff242b41;
                uint32_t edge=active?0xff667aaa:pointed?0xff52617f:0xff343d55;
                wm_draw_button(min_y,max_y,card_x,top,card_w,card_h,fill,fill,edge,false);
                if(active)wm_draw_button(min_y,max_y,card_x+1,top+5,3,card_h-10,0xffa6b9f0,0xffa6b9f0,0xffa6b9f0,false);
                const auto& preview=previews[i];
                int text_y=top+(card_h-(preview.lines[1][0]?2*line+3:line))/2;
                wm_draw_string(card_x+10,text_y,preview.lines[0],0xffeeeeff,100,preview_width);
                if(preview.lines[1][0])wm_draw_string(card_x+10,text_y+line+3,preview.lines[1],0xffeeeeff,100,preview_width);
                auto action=[&](int action_y,ClipboardIcon icon,uint32_t button_fill,uint32_t border,uint32_t ink){
                    int ax=panel.action_left(),ay=top+action_y;
                    wm_draw_button(min_y,max_y,ax,ay,CLIPBOARD_ACTION_WIDTH,CLIPBOARD_ACTION_HEIGHT,button_fill,button_fill,border,false);
                    constexpr int size=19;
                    static uint8_t masks[3][size][size]{};
                    static bool rasterized[3]{};
                    unsigned index=(unsigned)icon;
                    auto& mask=masks[index];
                    if(!rasterized[index]){
                        for(int row=0;row<size;++row)for(int col=0;col<size;++col)
                            mask[row][col]=(clipboard_icon_coverage(icon,col,row,size)*255+8)/16;
                        rasterized[index]=true;
                    }
                    int ix=ax+(CLIPBOARD_ACTION_WIDTH-size)/2,iy=ay+(CLIPBOARD_ACTION_HEIGHT-size)/2;
                    for(int row=0;row<size;++row){
                        int yy=iy+row;
                        if(yy<(int)min_y||yy>(int)max_y)continue;
                        auto* pixels=(uint32_t*)(g_master_backbuffer+(uint64_t)yy*fb->pitch)+ix;
                        for(int col=0;col<size;++col){
                            if(mask[row][col])pixels[col]=ui_font_blend(pixels[col],ink,mask[row][col]);
                        }
                    }
                };
                bool pin_enabled=preview.pinned||can_pin;
                bool pin_hover=pointed&&hover.action==ClipboardAction::Pin;
                bool delete_hover=pointed&&hover.action==ClipboardAction::Delete;
                bool pin_highlight=preview.pinned||(pin_hover&&pin_enabled);
                action(CLIPBOARD_PIN_TOP,preview.pinned?ClipboardIcon::Pinned:ClipboardIcon::Pin,
                       pin_highlight?0xff475980:fill,pin_highlight?0xff7186ba:edge,
                       pin_enabled?0xffe4ebff:0xff9098ae);
                action(CLIPBOARD_DELETE_TOP,ClipboardIcon::Delete,delete_hover?0xff59364a:fill,
                       delete_hover?0xffa46780:edge,delete_hover?0xffffc7d8:0xffdedff0);
                if(pin_hover)hint=preview.pinned?"Unpin entry":can_pin?"Pin entry":"Pin limit reached";
                if(delete_hover)hint="Delete entry";
            }
            if(total>panel.rows){
                int track=panel.rows*CLIPBOARD_ROW-CLIPBOARD_GAP,thumb=track*panel.rows/total;
                int rail_x=panel.card_right()+4,rail_y=panel.list_top();
                int offset=(track-thumb)*first/(total-panel.rows);
                wm_draw_button(min_y,max_y,rail_x,rail_y,3,track,0xff293148,0xff293148,0xff293148,false);
                wm_draw_button(min_y,max_y,rail_x,rail_y+offset,3,thumb,0xff7b8cb7,0xff7b8cb7,0xff7b8cb7,false);
            }
            if(count)wm_draw_string(card_x,y+panel.h-(CLIPBOARD_FOOTER+line)/2,
                                    hint,0xffb9c5ee,100,w-24);
        }
    }
    t2 = rdtsc_val();
    g_time_win_ticks = t2 - t1;
    }

    // Boot keeps the splash in VRAM until every part of the desktop is ready.
    // No cursor, overlay, GPU hook, or framebuffer write may precede this gate.
    if (!present) return false;

    // Present one complete scene after the final damage rectangle, including
    // cursor/ghost-only updates. Never copy partial scenes into live scanout.
    if(g_gpu_present_hook && g_desktop_ready){
        g_gpu_present_pending=true;
        if(!do_overlay)return false;
        uint64_t started=rdtsc_val();
        while(__sync_lock_test_and_set(&g_vram_lock,1))__asm__ volatile("pause");
        OverlayFrame frame;wm_capture_overlay(&frame,{g_mouse_x,g_mouse_y,wm_cursor_edges(g_mouse_x,g_mouse_y),g_mouse_buttons});
        wm_overlay_present_unlocked(frame,true);
        bool success=g_gpu_present_hook!=nullptr;
        __sync_lock_release(&g_vram_lock);
        if(!success){min_x=0;min_y=0;max_x=fb->width-1;max_y=fb->height-1;goto retry_scene;}
        g_time_vram_ticks=rdtsc_val()-started;
        return true;
    }

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
    static uint32_t cursor_save_buf[CURSOR_SPRITE_SIZE * CURSOR_SPRITE_SIZE];
    unsigned cursor_edges=wm_cursor_edges(ccx,ccy);
    OverlayFrame overlays;wm_capture_overlay(&overlays,{ccx,ccy,cursor_edges,cmb});
    for(int y=0;y<CURSOR_SPRITE_SIZE;++y){
        int sy=ccy+y-CURSOR_SPRITE_ORIGIN;if(sy<0||sy>=(int)fb->height)continue;
        auto* pixels=(uint32_t*)(g_master_backbuffer+(uint64_t)sy*ptch);
        for(int x=0;x<CURSOR_SPRITE_SIZE;++x){
            int sx=ccx+x-CURSOR_SPRITE_ORIGIN;if(sx<0||sx>=(int)fb->width)continue;
            cursor_save_buf[y*CURSOR_SPRITE_SIZE+x]=pixels[sx];
            uint32_t color=wm_cursor_color(cursor_edges,x-CURSOR_SPRITE_ORIGIN,y-CURSOR_SPRITE_ORIGIN,cmb);
            if(color)pixels[sx]=color;
        }
    }

    // ── Blit dirty rect (64-byte aligned for max throughput) ───────────────
    uint32_t min_x_aligned = min_x & ~15u; // 16-byte align for SSE (64 would be safer but might over-copy too much)
    uint32_t max_x_aligned = (max_x + 1 + 15) & ~15u;
    if (max_x_aligned > fb->width) max_x_aligned = fb->width;
    uint32_t bw_aligned = (max_x_aligned - min_x_aligned) * 4;

    // GPU path: one blitter copy of the whole dirty rect (synchronous: returns after the
    // fence, so the cursor restore below cannot race the blit). Falls back to the CPU
    // loop if the hook is unset or the GPU reported a problem. Active outlines
    // use the CPU span compositor so each blit includes their final pixels.
    bool gpu_done = false;
    if (g_gpu_flip_hook && !overlays.count) gpu_done = g_gpu_flip_hook(min_x_aligned, min_y, max_x_aligned, max_y + 1);
    if (!gpu_done) {
        for (uint32_t y = min_y; y <= max_y; y++) {
            uint8_t* dst = (uint8_t*)fb->address + (uint64_t)y * ptch + (uint64_t)min_x_aligned * 4;
            uint8_t* src = g_master_backbuffer   + (uint64_t)y * ptch + (uint64_t)min_x_aligned * 4;
            if(overlays.count)vram_present_span(min_x_aligned,max_x_aligned,y,overlays);
            else memcpy_vram_sse_headless(dst, src, bw_aligned);
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
    for(int y=0;y<CURSOR_SPRITE_SIZE;++y){
        int sy=ccy+y-CURSOR_SPRITE_ORIGIN;if(sy<0||sy>=(int)fb->height)continue;
        auto* pixels=(uint32_t*)(g_master_backbuffer+(uint64_t)sy*ptch);
        for(int x=0;x<CURSOR_SPRITE_SIZE;++x){
            int sx=ccx+x-CURSOR_SPRITE_ORIGIN;
            if(sx>=0&&sx<(int)fb->width)pixels[sx]=cursor_save_buf[y*CURSOR_SPRITE_SIZE+x];
        }
    }

    // Each dirty blit can introduce a new outline snapshot. Transition and
    // remember that exact snapshot even when this is not the final dirty rect.
    // Otherwise a later snapshot can leave fragments of this one untracked.
    wm_overlay_present_unlocked(overlays,true);
    __sync_lock_release(&g_vram_lock);
    return do_overlay;
}

// Split the HUD only when the lower band can use the opaque retained path.
// Ordered alpha/shadow scenes already need layer ordering; splitting those
// repeats engine waits and blending without making a retained path eligible.
// These are sequential calls, not recursion into the large compositor frame.
static inline bool wm_compose_dirty(uint32_t min_x,uint32_t max_x,
                                    uint32_t min_y,uint32_t max_y,bool do_overlay=true,
                                    bool present=true) {
    bool split_hud=true;
    if(present&&g_gpu_compose_hook&&min_y<60&&max_y>=60){
        TtyIrqGuard guard;
        for(int i=0;i<g_window_count;++i){
            auto* w=g_window_list[i];
            if(w&&w->visible&&!w->minimized&&
               (w->maximized||w->premultiplied||w->shadow_enabled)){split_hud=false;break;}
        }
    }else split_hud=false;
    if(split_hud){
        wm_compose_dirty_band(min_x,max_x,min_y,59,false,present);
        uint64_t bg=g_time_bg_ticks,win=g_time_win_ticks,rain=g_time_rain_ticks;
        uint64_t lock=g_time_lock_ticks,vram=g_time_vram_ticks;
        bool result=wm_compose_dirty_band(min_x,max_x,60,max_y,do_overlay,present);
        g_time_bg_ticks+=bg;g_time_win_ticks+=win;g_time_rain_ticks+=rain;
        g_time_lock_ticks+=lock;g_time_vram_ticks+=vram;
        return result;
    }
    return wm_compose_dirty_band(min_x,max_x,min_y,max_y,do_overlay,present);
}

// Shell bands keep their full-row repaint contract without forcing all the
// interior repairs back into separate scene submissions. Their Y ranges are
// disjoint from the exact interior clips, so no alpha pixel is blended twice.
static bool wm_compose_scene_repairs(const VisibleRegion& input){
    if(!fb||!gpu_direct_active()||!g_gpu_compose_hook||g_clipboard_open||g_taskbar_popup||
       (g_menu_open&&!wm_retained_menu_ready()))return false;
    static VisibleRegion canonical{},region{};VisibleRect bounds{};
    if(!gpu_scene_repair_region(&canonical,input,{0,0,(int)fb->width,(int)fb->height},&bounds))return false;
    int bottom=fb->height-g_taskbar_h,top_end=0,bottom_start=fb->height;
    region.count=0;
    for(unsigned i=0;i<canonical.count;++i){auto r=canonical.rects[i];
        if(r.y1<60&&r.y2>top_end)top_end=r.y2<60?r.y2:60;
        if(r.y2>bottom&&r.y1<bottom_start)bottom_start=r.y1>bottom?r.y1:bottom;
        r=visibility_intersect(r,{0,60,(int)fb->width,bottom});
        if(visibility_valid(r))region.rects[region.count++]=r;
    }
    uint64_t bg=0,win=0,rain=0,lock=0,vram=0;
    auto collect=[&](){bg+=g_time_bg_ticks;win+=g_time_win_ticks;rain+=g_time_rain_ticks;
        lock+=g_time_lock_ticks;vram+=g_time_vram_ticks;};
    if(top_end){wm_compose_dirty(0,fb->width-1,0,top_end-1,false);collect();}
    if(bottom_start<(int)fb->height){wm_compose_dirty(0,fb->width-1,bottom_start,fb->height-1,false);collect();}
    if(region.count){
        if(gpu_direct_active()){
            gpu_animation_damage_bounds(region,&bounds);
            ++g_gpu_perf.scene_repair_batches;g_gpu_perf.scene_repair_rects+=region.count;
            wm_compose_dirty_band(bounds.x1,bounds.x2-1,bounds.y1,bounds.y2-1,false,true,&region);collect();
        }else for(unsigned i=0;i<region.count;++i){
            // A shell-band GPU failure has already restored the CPU master.
            auto r=region.rects[i];wm_compose_dirty(r.x1,r.x2-1,r.y1,r.y2-1,false);collect();
        }
    }
    g_time_bg_ticks=bg;g_time_win_ticks=win;g_time_rain_ticks=rain;
    g_time_lock_ticks=lock;g_time_vram_ticks=vram;
    return true;
}

// Both ordinary retained repairs and a single transition rebase use this same
// ordered traversal. No presentation occurs until repairs and animation finish.
static void wm_compose_retained_repairs(const VisibleRegion& repairs){
    static VisibleRegion scene{};scene.count=0;
    for(unsigned i=0;i<repairs.count;++i)
        if(!wm_repair_retained_foreground(repairs.rects[i]))scene.rects[scene.count++]=repairs.rects[i];
    uint64_t bg=0,win=0,rain=0,lock=0,vram=0;
    auto collect=[&](){bg+=g_time_bg_ticks;win+=g_time_win_ticks;rain+=g_time_rain_ticks;
        lock+=g_time_lock_ticks;vram+=g_time_vram_ticks;};
    if(scene.count){
        if(wm_compose_scene_repairs(scene))collect();
        else for(unsigned i=0;i<scene.count;++i){auto r=scene.rects[i];
            wm_compose_dirty(r.x1,r.x2-1,r.y1,r.y2-1,false);collect();
        }
    }
    g_time_bg_ticks=bg;g_time_win_ticks=win;g_time_rain_ticks=rain;
    g_time_lock_ticks=lock;g_time_vram_ticks=vram;
}

#endif // WM_H
