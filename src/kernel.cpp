#include <stddef.h>
#include <stdbool.h>

#include "core/helpers.h"
#include "arch/pagingstuff.h"
#include "drivers/usbhelpers.h"

#include "arch/gdt.h"
#include "arch/idt.h"
#include "arch/cpu_power.h"

#include "arch/irq.h"    // PIC remapping + irq_register
#include "arch/timer.h"  // PIT driver + g_tick_count
#include "arch/rtc.h"    // CMOS RTC wall clock (taskbar clock)
#include "core/heap.h"
#include "core/tty.h"
#include "arch/acpi.h"
#include "core/syscall.h"
#include "fs/ramfs.h"
#include "fs/recovery.h"
#include "arch/boot_module.h"
#include "fs/ext4_vfs.h"
#include "fs/usb_autogrow.h"
#include "fs/fat32_vfs.h"

extern "C" void* wyzar_ext4_malloc(size_t n) { return kmalloc(n); }
extern "C" void* wyzar_ext4_realloc(void* p, size_t n) { return krealloc(p, n); }
extern "C" void wyzar_ext4_free(void* p) { kfree(p); }
#include "fs/tar.h"
#include "core/exec.h"
#include "gui/graphics.h"
#include "gui/wm.h"
#include "drivers/intel_gpu.h"
#if GPU_RENDER && GPU_TRACE
#include "drivers/gpu_pci_power_trace.h"
#endif
#include "drivers/intel_render.h"
#ifdef GPU_COMPOSITOR_TEST
#include "../tests/gpu_compositor_backend.h"
#endif
#include "arch/apic_timer.h"
#include "drivers/power_diag_collect.h"

// Each scanout retains a complete scene and accumulates all changes since its
// last flip. Rebuild its pending damage before presenting it again.
static bool begin_direct_scanout(DamageList& damage,VisibleRegion* exact_damage=nullptr,bool retained_animation=false){
    if(!GPU_RCS_ALPHA)return false;
    using Check=ScanoutCheck;
    if(!g_gpu_direct_verified)return reject_scanout(Check::Readback);
    if(!g_gpu_present_ready)return reject_scanout(Check::Present);
    if(!g_gpu_ready)return reject_scanout(Check::Gpu);
    if(!g_render_ready)return reject_scanout(Check::Render);
    if(g_gpu_present_hook!=gpu_present_frame)return reject_scanout(Check::PresentHook);
    if(g_gpu_compose_hook!=gpu_compose_scene)return reject_scanout(Check::ComposeHook);
    if(g_gpu_render_hook!=gpu_render_scene)return reject_scanout(Check::RenderHook);
    if(g_cpu_alpha_pixel_limit)return reject_scanout(Check::Policy);
    int back=g_gpu_display_flip.writable();
    if(back<0)return reject_scanout(Check::Back);
    if(g_gpu_display_flip.front<0)return reject_scanout(Check::Front);
    if(fb->address==g_gpu_scanout_cpu[back])return reject_scanout(Check::Alias);
    uint32_t front=g_gpu_display_flip.surfaces[g_gpu_display_flip.front];
    auto plane_base=[](uint32_t reg){
        return *(volatile uint32_t*)(g_igpu.mmio_virt+reg)&0xfffff000u;
    };
    uint32_t live=plane_base(g_gpu_display_plane.live);
    if(live!=front)return reject_scanout(Check::Live,live,front);
    uint32_t armed=plane_base(g_gpu_display_plane.surface);
    if(armed!=front)return reject_scanout(Check::Surface,armed,front);
    // Shell popups obey exact clips in both cached and fallback painting.
    // Keep client animation repairs bounded even when no Rainbow is running.
    bool popup=g_menu_open&&!wm_retained_menu_ready();
#if GPU_EXACT_REPAIRS
    int tray_left=retained_animation&&!popup?wm_tray_damage_left():-1;
    if(exact_damage){
#if GPU_FRAME_DIAGNOSTICS
        GpuCpuPhaseTimer cost(gpu_phase_cost(GpuFrameTimeline::Merge),wm_trace_cpu_cycles);
#endif
        VisibleRegion changed{};
        bool stats_visible;
        {
            TtyIrqGuard guard;
            stats_visible=wm_desktop_stats_visible();
        }
        gpu_exact_frame_damage(changed,damage,fb->width,fb->height,g_taskbar_h,popup,stats_visible,tray_left);
        for(unsigned buffer=0;buffer<2;++buffer)for(unsigned i=0;i<changed.count;++i){
            if(!visibility_add_exact(&g_gpu_scanout_exact_damage[buffer],changed.rects[i])){
                g_gpu_scanout_exact_damage[buffer].count=1;
                g_gpu_scanout_exact_damage[buffer].rects[0]={0,0,(int)fb->width,(int)fb->height};break;
            }
        }
    }else
#endif
    {
    DamageList changed{};
    gpu_frame_damage(changed,damage,fb->width,fb->height,g_taskbar_h,
                     popup);
    for(unsigned buffer=0;buffer<2;++buffer){
        for(unsigned i=0;i<changed.n;++i){
            const auto& r=changed.r[i];
            damage_add(&g_gpu_scanout_damage[buffer],r.x1,r.x2,r.y1,r.y2);
        }
    }
    }
    auto* hidden=g_gpu_scanout_cpu[back];
    if(!g_gpu_scanout_scene_valid[back]){
        // Seed a complete scene once. Pending damage includes every update
        // since the retained master last presented, including other-buffer work.
        memcpy_vram_sse_headless(hidden,g_master_backbuffer,(uint64_t)fb->pitch*fb->height);
        g_gpu_perf.scanout_seed_bytes+=(uint64_t)fb->pitch*fb->height;
        g_gpu_scanout_scene_valid[back]=true;
#if GPU_CURSOR_SAVE
    }else if(g_gpu_cursor_save[back].restore(hidden,fb->width,fb->height,fb->pitch)){
        ++g_gpu_perf.cursor_restores;
#endif
    }else{
        // Recompose the old overlay's pixels as ordinary scene damage. This
        // also works after a window moves or closes, without reading stale
        // pixels from the retained master or the live display surface.
        const OverlayFrame empty{{-999,-999,0,0},{},0};
        DamageList overlay{};
        overlay_transition(g_gpu_scanout_overlay[back],empty,fb->height,[&](int left,int right,int y){
            if(left<0)left=0;if(right>(int)fb->width)right=fb->width;
            if(left<right)damage_add_clipped(&overlay,fb->width,fb->height,left,right-1,y,y);
        },false);
#if GPU_EXACT_REPAIRS
        if(exact_damage){
#if GPU_FRAME_DIAGNOSTICS
            GpuCpuPhaseTimer cost(gpu_phase_cost(GpuFrameTimeline::Merge),wm_trace_cpu_cycles);
#endif
            bool stats_visible;
            {
                TtyIrqGuard guard;
                stats_visible=wm_desktop_stats_visible();
            }
            gpu_exact_frame_damage(g_gpu_scanout_exact_damage[back],overlay,
                fb->width,fb->height,g_taskbar_h,popup,stats_visible,tray_left);
        }
        else
#endif
        gpu_frame_damage(g_gpu_scanout_damage[back],overlay,fb->width,fb->height,g_taskbar_h,popup);
    }
    // Overflow merges can grow into shell rows. Normalize the final list too.
    damage_clear(&damage);
#if GPU_EXACT_REPAIRS
    if(exact_damage){
#if GPU_FRAME_DIAGNOSTICS
        GpuCpuPhaseTimer cost(gpu_phase_cost(GpuFrameTimeline::Pack),wm_trace_cpu_cycles);
#endif
        exact_damage->count=g_gpu_scanout_exact_damage[back].count;
        for(unsigned i=0;i<exact_damage->count;++i){
            auto r=g_gpu_scanout_exact_damage[back].rects[i];exact_damage->rects[i]=r;
            damage_add(&damage,r.x1,r.x2-1,r.y1,r.y2-1);
        }
    }else
#endif
    gpu_frame_damage(damage,g_gpu_scanout_damage[back],fb->width,fb->height,g_taskbar_h,
                     popup);
    g_gpu_perf.repair_rects+=damage.n;
    for(unsigned i=0;i<damage.n;++i)g_gpu_perf.repair_pixels+=dmg_area(damage.r[i]);
    gpu_mfence();
    g_gpu_retained_master=g_master_backbuffer;
    g_gpu_direct_back=back;
    g_master_backbuffer=hidden;
    return true;
}

#if GPU_FRAME_DIAGNOSTICS
static void log_frame_gap(const GpuFrameTimeline& frame) {
    if(!gpu_frame_gap_due(frame.interval(),frame.refreshes,tsc_hz))return;
    // Keep continuous deadline misses from filling the boot log. Longer
    // interruptions have a separate budget from ordinary missed refreshes.
    const bool long_gap=(frame.refreshes>=3&&frame.refreshes<0x80000000u)||frame.interval()>=tsc_hz/20;
    static uint64_t last_short=0;
    static unsigned short_count=0,long_count=0;
    if(long_gap){
        if(long_count>=48)return;
        ++long_count;
    }else{
        if(short_count>=48||(last_short&&frame.latch-last_short<tsc_hz/4))return;
        last_short=frame.latch;
        ++short_count;
    }
    char line[512];char* end=line;
    auto text=[&](const char* value){while(*value&&end<line+sizeof(line)-2)*end++=*value++;};
    auto number=[&](uint64_t value){char digits[24];to_str(value,digits);text(digits);};
    auto duration=[&](const char* label,uint64_t ticks){text(label);number(ticks*1000000/tsc_hz);};
    auto timestamp=[&](const char* label,uint64_t tick){
        text(label);
        if(!tick){text("-");return;}
        if(tick<frame.previous_latch){text("-");number((frame.previous_latch-tick)*1000000/tsc_hz);}
        else number((tick-frame.previous_latch)*1000000/tsc_hz);
    };
    auto emit=[&]{
        *end++='\n';*end=0;
        TtyIrqGuard guard;
        klog_print(line);
        end=line;
    };
    text("GPU frame gap: frame=");number(g_compositor_frame);
    text(" menu=");number(g_menu_open);
    duration(" intervalUs=",frame.interval());text(" refreshes=");number(frame.refreshes);
    timestamp(" composeAtUs=",frame.compose);timestamp(" submitAtUs=",frame.first_submit);
    timestamp(" readyAtUs=",frame.last_ready);timestamp(" armAtUs=",frame.arm);
    duration(" cpuUs=",frame.cpu);duration(" bcsUs=",frame.bcs);duration(" rcsUs=",frame.rcs);
    text(" bcsBatches=");number(frame.bcs_batches);text(" rcsDraws=");number(frame.rcs_draws);
    emit();
    text("GPU gap costs: frame=");number(g_compositor_frame);
    duration(" repairsUs=",frame.planning[GpuFrameTimeline::RepairPaint].wall);
    duration(" fallbackUs=",frame.planning[GpuFrameTimeline::Fallback].wall);
    duration(" menuBuildUs=",frame.planning[GpuFrameTimeline::MenuBuild].wall);
    duration(" menuReplayUs=",frame.planning[GpuFrameTimeline::MenuReplay].wall);
    duration(" maxFlipSleepUs=",frame.max_flip_sleep);
    duration(" sleepToReadyUs=",frame.sleep_to_ready);
    duration(" readyToResumeUs=",frame.ready_to_resume);
    text(" readyPid=");if(frame.sleep_ready_pid<0)text("-");else number(frame.sleep_ready_pid);
    emit();
    const auto& rcs=frame.slowest_rcs;
    if(rcs.submitted){
        text("GPU gap RCS: frame=");number(g_compositor_frame);
        text(" seq=");number(rcs.sequence);
        timestamp(" submittedAtUs=",rcs.submitted);timestamp(" observedAtUs=",rcs.observed);
        duration(" waitUs=",rcs.wait_ticks);
        const auto& hw=rcs.hardware;
        text(" stamp=");text(hw.samples?"valid":hw.invalid?"invalid":"unavailable");
        if(hw.samples){
            text(" toDrainUs=");number(hw.queue_us+hw.setup_us+hw.parse_us+hw.rt_us+hw.drain_us);
            text(" drainToObserveUs=");number(hw.tail_us);
        }
        duration(" maxSleepUs=",rcs.max_sleep);
        duration(" sleepToReadyUs=",rcs.sleep_to_ready);
        duration(" readyToResumeUs=",rcs.ready_to_resume);
        text(" readyPid=");if(rcs.sleep_ready_pid<0)text("-");else number(rcs.sleep_ready_pid);
        emit();
    }

    uint64_t latest;
    {TtyIrqGuard guard;latest=g_gpu_gap_history.latest;}
    constexpr const char* activities[]={"load","resources","catalog-publish","start-open","start-close"};
    unsigned reported=0;
    for(unsigned offset=0;offset<GpuGapHistory::capacity&&offset<latest&&reported<6;++offset){
        uint64_t serial=latest-offset;
        GpuGapEvent event;
        {
            TtyIrqGuard guard;
            event=g_gpu_gap_history.events[GpuGapHistory::slot(serial)];
        }
        if(event.serial!=serial||!event.overlaps(frame.previous_latch,frame.latch))continue;
        text("GPU gap activity: frame=");number(g_compositor_frame);
        text(" activity=");text(activities[(unsigned)event.activity]);
        text(" pid=");if(event.pid<0)text("-");else number(event.pid);
        text(" irqEnabled=");number(event.irq_enabled);
        timestamp(" beginAtUs=",event.begin);timestamp(" endAtUs=",event.end);
        if(event.detail[0]){text(" detail=");text(event.detail);}
        emit();
        ++reported;
    }
}
#endif

static void log_app_launches(uint64_t now){
    if(!GPU_RCS_ALPHA || !tsc_hz)return;
    for(unsigned i=0;i<g_gpu_launch_count;++i){
        auto& trace=g_gpu_launches[i];
        if(trace.printed || now-trace.started<tsc_hz ||
           (!trace.displayed && now-trace.started<tsc_hz*5))continue;
        trace.printed=true;
        char line[512];char* end=line;
        auto text=[&](const char* value){while(*value && end<line+sizeof(line)-1)*end++=*value++;};
        auto number=[&](uint64_t value){char digits[24];to_str(value,digits);text(digits);};
        auto phase=[&](const char* label,uint64_t tick){
            text(label);if(tick)number((tick-trace.started)*1000000/tsc_hz);else text("-");
        };
        text("GPU launch: path=");text(trace.path);text(" window=");number(trace.window);
        phase(" readyUs=",trace.ready);phase(" createdUs=",trace.created);
        phase(" paintedUs=",trace.painted);phase(" composedFlipUs=",trace.displayed);
        text(" maxFlipUs=");number(trace.max_interval*1000000/tsc_hz);
        text(" directFlips=");number(trace.direct);text(" copiedFlips=");number(trace.copied);
        *end=0;print(line);
    }
}

static void log_menu_opens(uint64_t now){
    if(!GPU_RCS_ALPHA || !tsc_hz)return;
    for(unsigned i=0;i<g_menu_open_trace_count;++i){
        auto& trace=g_menu_open_traces[i];
        if(trace.printed || now-trace.opened<tsc_hz)return;
        trace.printed=true;
        char line[512];char* end=line;
        auto text=[&](const char* value){while(*value && end<line+sizeof(line)-1)*end++=*value++;};
        auto number=[&](uint64_t value){char digits[24];to_str(value,digits);text(digits);};
        auto us=[&](uint64_t ticks){number(ticks*1000000/tsc_hz);};
        auto phase=[&](const char* label,uint64_t tick){
            text(label);
            if(!tick){text("-");return;}
            if(tick<trace.opened){text("-");us(trace.opened-tick);}
            else us(tick-trace.opened);
        };
        text("GPU menu-open: id=");number(i+1);text(" refresh=");number(trace.refresh);
        phase(" requestedUs=",trace.requested);phase(" resourcesStartUs=",trace.resource_start);
        phase(" resourcesDoneUs=",trace.resource_done);phase(" resourcesCommitUs=",trace.resource_commit);
        phase(" cacheStartUs=",trace.cache_start);phase(" cacheDoneUs=",trace.cache_done);
        phase(" maxBcsStartUs=",trace.bcs_start);phase(" maxBcsDoneUs=",trace.bcs_done);
        phase(" firstPresentUs=",trace.first_present);
        text(" gpu=");number(trace.gpu);*end=0;print(line);
        end=line;text("GPU menu-cost: id=");number(i+1);
        text(" allocationUs=");us(trace.allocation_ticks);
        text(" buildUs=");us(trace.build_ticks);text(" resourcesCpuUs=");us(trace.resource_cpu);
        text(" maxBcsWallUs=");us(trace.bcs_wall);text(" maxBcsCpuUs=");us(trace.bcs_cpu);
        text(" maxBcsSleepUs=");us(trace.bcs_sleep);text(" maxPresentGapUs=");us(trace.max_interval);
        *end=0;print(line);
    }
}

static void process_exit_ui(pid_t pid) {
    hda::task_departed(pid);
    bool foreground = pid == g_terminal_foreground_pid;
    wm_destroy_task_windows(pid);
    if (foreground && g_win_terminal) {
        for (int i = 0; i < g_window_count; ++i)
            if (g_window_list[i] == g_win_terminal) { wm_raise_window(i); break; }
    }
}

static char desktop_launch_paths[8][256];
static unsigned desktop_launch_head=0,desktop_launch_tail=0;
static Task* desktop_launcher_task=nullptr;
static int desktop_power_action=0;
static void desktop_queue_power(int action) {
    TtyIrqGuard guard;
    desktop_power_action=action;
    if(desktop_launcher_task&&desktop_launcher_task->state==TaskState::BLOCKED)desktop_launcher_task->state=TaskState::READY;
}
static bool desktop_queue_launch(const char* path) {
    TtyIrqGuard guard;unsigned next=(desktop_launch_head+1)%8;
    if(next==desktop_launch_tail||!path||strlen(path)>=256)return false;
    strcpy(desktop_launch_paths[desktop_launch_head],path);desktop_launch_head=next;
    if(desktop_launcher_task&&desktop_launcher_task->state==TaskState::BLOCKED)desktop_launcher_task->state=TaskState::READY;
    return true;
}
static bool desktop_shortcut(char key,uint32_t mods,bool repeat) {
    if((key=='t'||key=='T')&&mods==(WIN_MOD_CTRL|WIN_MOD_ALT)){
        if(!repeat)desktop_queue_launch("/bin/init.elf");
        return true;
    }
    if(key!=27||mods!=(WIN_MOD_CTRL|WIN_MOD_SHIFT))return false;
    if(repeat)return true;
    TtyIrqGuard guard;
    if(g_menu_open)wm_menu_close();
    const char* path="/bin/task_manager.wiz";
    for(int i=0;i<g_window_count;++i){Window* win=g_window_list[i];
        if(win&&!win->is_system&&strcmp(win->executable,path)==0){
            wm_raise_window(i);return true;}}
    // A queued/starting instance may not own a window yet. Avoid duplicate
    // launches without reading the filesystem in the keyboard IRQ/dispatcher.
    Task* task=g_task_list;
    if(task)do{if(task->state!=TaskState::DEAD&&strcmp(task->executable,path)==0)return true;task=task->next;}while(task!=g_task_list);
    for(unsigned i=desktop_launch_tail;i!=desktop_launch_head;i=(i+1)%8)
        if(strcmp(desktop_launch_paths[i],path)==0)return true;
    if(!desktop_queue_launch(path))print((char*)"[launcher] Task Manager shortcut: launch queue is full.");
    return true;
}
static void desktop_launch_worker() {
    // A fresh kernel task enters from schedule() with IRQs masked.
    // USB reads must be preemptible, including the first queued launch.
    __asm__ volatile("sti" ::: "memory");
    for(;;){char path[256];int power=0;
        {TtyIrqGuard guard;
            while(desktop_launch_head==desktop_launch_tail&&!desktop_power_action){g_current_task->state=TaskState::BLOCKED;g_current_task->wake_at_tick=0;
                schedule();if(g_current_task->state==TaskState::BLOCKED)scheduler_idle_halt();}
            power=desktop_power_action;desktop_power_action=0;
            if(!power){strcpy(path,desktop_launch_paths[desktop_launch_tail]);desktop_launch_tail=(desktop_launch_tail+1)%8;}
        }
        // Filesystem shutdown may sleep; never run it in the pointer handler.
        if(power==MENU_SHUTDOWN){sys_shutdown();continue;}
        if(power==MENU_RESTART){storage_prepare_poweroff();hardware_reset();continue;}
        bool gui=false;if(launch_program(path,true,&gui)<0)print((char*)"[launcher] App launch failed.");
    }
}

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
static volatile struct limine_kernel_address_request kernel_address_req = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0
};
extern "C" char _kernel_start[], _kernel_end[];
static bool g_recovery_retired = false;
static uint64_t g_recovery_heap_bytes = 0, g_recovery_archive_bytes = 0;
static uint64_t g_recovery_fallback_bytes = 0;

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

// The scheduler runs on the BSP only. Do not request Limine SMP startup merely
// for a CPU count: it starts/parks APs and adds an unnecessary firmware/APIC
// dependency before kmain. The HUD already obtains topology through CPUID.

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
// Direct framebuffer helpers also serve the early loading ring and panic UI.
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

#include "gui/boot_splash.h"

static void storage_setup_progress(const char* message) {
    print((char*)message);
    if (!g_boot_splash_active || !fb) return;
    int32_t y = (int32_t)fb->height / 2 + 60;
    for (int32_t row = y; row < y + 12 && row < (int32_t)fb->height; ++row)
        for (uint32_t x = 0; x < fb->width; ++x) vram_putpx(x, row, BOOT_SPLASH_BG);
    int32_t x = ((int32_t)fb->width - (int32_t)strlen(message) * 8) / 2;
    if (x < 0) x = 0;
    vram_draw_string_8x8(x, y, message, 0xffc6d6ee);
    vram_fence();
}

// ── Syscall globals ───────────────────────────────────────────────────────────
alignas(16) static uint8_t g_boot_kstack[4096];
uint64_t g_current_kstack_top = (uint64_t)(g_boot_kstack + sizeof(g_boot_kstack));
uint64_t g_user_rsp_scratch = 0;

// ── klog globals ──────────────────────────────────────────────────────────────
#include "core/klog.h"
char     g_klog_buffer[KLOG_BUFFER_SIZE];
uint64_t g_klog_pos = 0;
// Keep normal startup quiet. Logs still populate /dev/klog and the WM viewer.
bool     g_klog_bypass_framebuffer = true;
bool     g_fatal_screen_visible = false;
uint8_t* g_backbuffer = nullptr;
uint8_t* g_master_backbuffer = nullptr;

// Phase 11: Speed King globals
volatile uint32_t g_dirty_min_y = 0xFFFFFFFF;
volatile uint32_t g_dirty_max_y = 0;
volatile uint32_t g_dirty_min_x = 0xFFFFFFFF;
volatile uint32_t g_dirty_max_x = 0;
DamageList g_damage;   // separate dirty rects; see damage.h

// Phase 9: WM post-flip hook (set to wm_render_chrome once WM is active)
post_flip_hook_fn g_post_flip_hook = nullptr;
#ifndef GPU_BRINGUP
#define GPU_BRINGUP 0   // set by the Makefile: make GPU=1
#endif
gpu_flip_hook_fn g_gpu_flip_hook = nullptr;
gpu_compose_hook_fn g_gpu_compose_hook = nullptr;
gpu_compose_hook_fn g_gpu_render_hook = nullptr;
gpu_present_hook_fn g_gpu_present_hook = nullptr;
bool g_gpu_quarantine_sources = false;
uint64_t g_gpu_surface_epoch = 0;
char g_gpu_status[64] = "GPU:none";
pre_flip_hook_fn  g_pre_flip_hook  = nullptr;
static uint64_t last_mouse_flip_tsc = 0;

// Phase 9: Terminal window origin + size (set by wm_init)
int32_t g_term_ox = 0;
int32_t g_term_oy = 0;
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
static volatile bool g_boot_assets_ready = false;
static uint64_t g_boot_started_tsc = 0;
static vnode* g_storage_report = nullptr;
static uint64_t g_storage_report_offset = 0;
static void storage_report_line(const char* line) {
    if (!g_storage_report || !g_storage_report->ops->write) return;
    uint64_t size = strlen(line);
    g_storage_report_offset += g_storage_report->ops->write(g_storage_report, line, size, g_storage_report_offset);
    g_storage_report_offset += g_storage_report->ops->write(g_storage_report, "\n", 1, g_storage_report_offset);
}
static void storage_record_recovery(vnode* runtime_run, const char* reason) {
    print((char*)"[storage] Recovery reason:");
    print((char*)reason);
    // Write directly to the runtime RAM node, never to the failing USB device.
    if (!runtime_run || runtime_run->type != VnodeType::DIRECTORY || runtime_run->ops != &ramfs_ops) return;
    g_storage_report = ramfs_lookup(runtime_run, "storage.txt");
    if (!g_storage_report) g_storage_report = ramfs_create(runtime_run, "storage.txt");
    if (!g_storage_report || g_storage_report->type != VnodeType::FILE) return;
    ((ramfs_node*)g_storage_report->fs_data)->size = g_storage_report->size = 0;
    g_storage_report_offset = 0;
    storage_report_line("System root: RAMFS recovery");
    storage_report_line(reason);
    usb_storage_failure_summary(storage_report_line);
}
volatile uint32_t g_mouse_btns_prev = 0;

#include "drivers/mouse.h"
#include "drivers/synaptics.h"
#include "drivers/ps2kb.h"

// Phase 18: High-precision TSC calibration against PIT
static bool calibrate_tsc_pit() {
    print((char*)"Calibrating TSC...");
    // Bound legacy timer detection so newer firmware can use the LAPIC path.
    const uint64_t timeout = _usb_tsc_hz() * 2;
    uint64_t deadline_start = rdtsc();
    
    // Wait for a PIT tick boundary (g_tick_count from timer.h)
    uint64_t start_tick = g_tick_count;
    while (g_tick_count == start_tick) {
        boot_splash_tick();
        if (rdtsc() - deadline_start > timeout) {
            print((char*)"[timer] No PIT IRQ0; trying local APIC timer...");
            return false;
        }
        __asm__ volatile("pause");
    }
    
    // Start measurement
    uint64_t tsc1 = rdtsc();
    uint64_t m_start = g_tick_count;
    
    // Measuring for 100ms (100 ticks) for high precision
    deadline_start = rdtsc();
    while (g_tick_count - m_start < 100) {
        boot_splash_tick();
        if (rdtsc() - deadline_start > timeout) {
            print((char*)"[timer] PIT IRQ0 stopped; trying local APIC timer...");
            return false;
        }
        __asm__ volatile("pause");
    }
    uint64_t tsc2 = rdtsc();
    
    uint64_t delta = tsc2 - tsc1;
    tsc_hz = delta * 10; // 100ms * 10 = 1 second
    
    char mstr[32];
    print((char*)" TSC Hz: "); to_str(tsc_hz, mstr); print(mstr);
    return true;
}

static void init_boot_timer() {
    init_irq();
    init_timer(1000);
    __asm__ volatile("sti" ::: "memory");
    if (calibrate_tsc_pit()) {
        print((char*)"[timer] PIT interrupts verified.");
        return;
    }
    __asm__ volatile("cli" ::: "memory");
    irq_unregister(0); // prevent double ticks if legacy IRQ0 arrives later
    if (init_local_apic_timer(tsc_hz)) {
        uint64_t first = g_tick_count;
        uint64_t start = rdtsc();
        __asm__ volatile("sti" ::: "memory");
        while (g_tick_count - first < 10 && rdtsc() - start < tsc_hz) {
            boot_splash_tick();
            __asm__ volatile("pause");
        }
        if (g_tick_count - first >= 10) {
            print((char*)"[timer] Local APIC interrupts verified. Boot continuing.");
            return;
        }
        __asm__ volatile("cli" ::: "memory");
        timer_apic_write(0x320, (1u << 16) | LOCAL_TIMER_VECTOR);
        timer_apic_write(0x380, 0);
        print((char*)"[timer] ERROR: Local APIC configured but interrupts did not arrive.");
    } else {
        print((char*)"[timer] ERROR: Could not calibrate a local APIC timer.");
    }
    print((char*)"[boot] ERROR: No working timer interrupt source. Halted.");
    hcf();
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
    if(g_taskbar_popup){wm_taskbar_scroll(scroll_dy);return;}
    if(g_clipboard_open){wm_clipboard_scroll(scroll_dy);return;}
    if (!fb) return;
    int32_t mx = g_mouse_x, my = g_mouse_y;
    int idx = wm_find_top_window(mx, my);
    if (idx != -1) {
        Window* win = g_window_list[idx];
        if (win->is_system && win->type == WIN_TYPE_TERM) {
            term_scroll_view(scroll_dy);
        } else if (win->is_system && win->type == WIN_TYPE_LOG) {
            wm_log_scroll(scroll_dy);
        } else {
            WinEvent ev;
            ev.type = WIN_EVENT_MOUSE_SCROLL;
            wm_event_coordinates(win,mx,my,&ev);
            ev.key = 0;
            ev.mods = g_kbd_modifiers;
            ev.delta = scroll_dy;
            wm_win_push_event(win, ev);
        }
    }
}

extern "C" void mouse_process_input(int16_t dx, int16_t dy, uint8_t buttons) {
    if (!fb) return;
    ResourceRead resource_read; // Keep hit-test identity and launch path in one snapshot.

    g_mouse_prev_buttons = (uint8_t)g_mouse_buttons;
    g_mouse_buttons      = buttons;
    bool btn_held     = (g_mouse_buttons  & 1);
    bool btn_was_held = (g_mouse_prev_buttons & 1);
    bool mouse_press  = btn_held && !btn_was_held;
    bool mouse_release= !btn_held && btn_was_held;
    static bool clipboard_press=false;

    // 1. Move cursor (clamped to screen)
    auto clipboard_hover=wm_clipboard_hit(g_mouse_x,g_mouse_y);
    int previous_menu_item=g_menu_open?wm_menu_hit_test(g_mouse_x,g_mouse_y):-1;
    int previous_popup_item=wm_taskbar_popup_hover(g_mouse_x,g_mouse_y);
    g_mouse_x += dx;
    g_mouse_y += dy;
    if (dx != 0 || dy != 0 || buttons != g_mouse_prev_buttons) g_needs_refresh = true;

    if (g_mouse_x < 0) g_mouse_x = 0;
    if (g_mouse_x >= (int32_t)fb->width)  g_mouse_x = fb->width  - 1;
    if (g_mouse_y < 0) g_mouse_y = 0;
    if (g_mouse_y >= (int32_t)fb->height) g_mouse_y = fb->height - 1;
    if((dx||dy)&&g_clipboard_open&&clipboard_hover!=wm_clipboard_hit(g_mouse_x,g_mouse_y))
        wm_clipboard_damage();

    // Start menu hover highlight follows the pointer
    if (g_menu_open && (dx != 0 || dy != 0)) wm_menu_hover_dirty(previous_menu_item,wm_menu_hit_test(g_mouse_x,g_mouse_y));
    if(g_taskbar_popup&&(dx||dy)&&previous_popup_item!=wm_taskbar_popup_hover(g_mouse_x,g_mouse_y))wm_taskbar_damage(false);

    // 2. Start drag / Raise / Close / Taskbar: hit-test Z-stack on fresh click
    if(mouse_press)clipboard_press=wm_clipboard_click(g_mouse_x,g_mouse_y)||wm_taskbar_popup_click(g_mouse_x,g_mouse_y);
    if(!mouse_press&&wm_volume_pointer(btn_held))clipboard_press=true;
    if (mouse_press && !clipboard_press) {
        int32_t mx = g_mouse_x, my = g_mouse_y;
        
        // Taskbar / Start button
        if (my >= (int32_t)fb->height - (int32_t)g_taskbar_h) {
            if (mx >= 6 && mx <= (int32_t)(6 + g_sb_w)) {
                // Start button toggles the menu
                if (g_menu_open) wm_menu_close();
                else            wm_menu_open();
            } else {
                if (g_menu_open) wm_menu_close(); // any other taskbar click dismisses it
                wm_taskbar_click_bar(mx);
            }
        } else {
            bool in_menu=false;
            int menu_hit = wm_menu_hit_test(mx, my, &in_menu);
            if(menu_hit==MENU_RESTART||menu_hit==MENU_SHUTDOWN) {
                wm_menu_close();
                desktop_queue_power(menu_hit);
            } else if (menu_hit >= 0 && !g_resources_catalog->apps[menu_hit].app.available) {
                // Pinned app whose file or drive is missing: nothing to launch.
                print((char*)"[launcher] That app is unavailable (its file or drive is missing).");
                wm_menu_close();
            } else if (menu_hit >= 0) {
                // Launch the selected app (installed, pinned, or found in /bin or /home/Applications)
                if (!desktop_queue_launch(g_resources_catalog->apps[menu_hit].app.exec))
                    print((char*)"[launcher] Launch queue is full.");
                wm_menu_close();
            } else if (g_menu_open) {
                if(!in_menu)wm_menu_close();
            } else {
            int idx = wm_find_top_window(mx, my);
            if (idx != -1) {
                wm_raise_window(idx);
                Window* win = g_window_list[idx];
                if (wm_is_in_close(idx, mx, my)) {
                    wm_close_window(idx);
                } else if(window_control_hit(win->x,win->y-g_title_h,win->w,g_title_h,mx,my)==WINDOW_MINIMIZE) {
                    wm_minimize_window(idx);
                } else if(window_control_hit(win->x,win->y-g_title_h,win->w,g_title_h,mx,my)==WINDOW_MAXIMIZE) {
                    wm_toggle_maximize(win);
                } else if(unsigned edges=win->maximized?0:resize_hit({win->x,win->y,win->w,win->h},g_title_h,mx,my)) {
                    win->resize_edges=edges;win->resize_original={win->x,win->y,win->w,win->h};
                    win->resize_mouse_x=mx;win->resize_mouse_y=my;
                    win->ghost_x=win->x;win->ghost_y=win->y;win->ghost_w=win->w;win->ghost_h=win->h;
                } else if (wm_is_in_title(idx, mx, my)) {
                    static uint64_t last_title_click=0;static int last_title_id=-1,last_title_x=0,last_title_y=0;
                    uint64_t now=rdtsc();int tx=mx-last_title_x,ty=my-last_title_y;
                    if(last_title_id==win->id&&now-last_title_click<_usb_tsc_hz()*4/10&&tx>=-5&&tx<=5&&ty>=-5&&ty<=5){
                        wm_toggle_maximize(win);last_title_id=-1;
                    }else {
                        last_title_click=now;last_title_id=win->id;last_title_x=mx;last_title_y=my;
                        if(win->maximized){win->restore_drag_pending=true;
                            win->resize_mouse_x=mx;win->resize_mouse_y=my;
                            win->drag_off_x=mx-win->x;win->drag_off_y=my-(win->y-(int)g_title_h);
                        }else {win->ghost_dragging=true;win->ghost_x=win->x;win->ghost_y=win->y;
                            win->drag_off_x=mx-win->x;win->drag_off_y=my-win->y;}
                    }
                } else if(!wm_is_in_title(idx,mx,my)) {
                    WinEvent ev;
                    ev.type = WIN_EVENT_MOUSE_DOWN;
                    wm_event_coordinates(win,mx,my,&ev);
                    ev.key = 1; // Left button
                    ev.mods = g_kbd_modifiers;
                    ev.delta = 0;
                    wm_win_push_event(win, ev);
                }
            } else {
                // Clicked on desktop background
                g_focused_win_id = -1;
                g_dirty_min_x = 0; g_dirty_max_x = fb->width - 1;
                g_dirty_min_y = 0; g_dirty_max_y = fb->height - 1;
                g_needs_refresh = true;
            }
            }
        }
    }

    // Secondary buttons go to native clients without starting title/resize drags.
    for(unsigned button=2;!g_clipboard_open&&button<=4;button<<=1){
        if((g_mouse_buttons&button)==(g_mouse_prev_buttons&button)||g_menu_open||g_taskbar_popup)continue;
        int idx=wm_find_top_window(g_mouse_x,g_mouse_y);
        if(idx<0)continue;Window* win=g_window_list[idx];
        if(win->is_system||g_mouse_y<win->y||g_mouse_y>=win->y+win->h)continue;
        if(g_mouse_buttons&button)wm_raise_window(idx);
        WinEvent ev{};ev.type=(g_mouse_buttons&button)?WIN_EVENT_MOUSE_DOWN:WIN_EVENT_MOUSE_UP;
        wm_event_coordinates(win,g_mouse_x,g_mouse_y,&ev);ev.key=button;ev.mods=g_kbd_modifiers;
        wm_win_push_event(win,ev);
    }

    // 3. Restore a maximized title drag, then update the drag/resize outline.
    for (int i = 0; i < g_window_count; i++) {
        Window* win = g_window_list[i];
        if(win&&win->restore_drag_pending&&btn_held){
            int move_x=g_mouse_x-win->resize_mouse_x,move_y=g_mouse_y-win->resize_mouse_y;
            if(move_x<=-4||move_x>=4||move_y<=-4||move_y>=4){
                ResizeRect target=restore_drag_rect(win->restore_rect,win->w,win->drag_off_x,win->drag_off_y,
                    g_mouse_x,g_mouse_y,g_title_h,fb->width);
                wm_toggle_maximize(win);
                win->restore_drag_pending=false;win->ghost_dragging=true;
                win->drag_off_x=g_mouse_x-target.x;win->drag_off_y=g_mouse_y-target.y;
            }
        }
        if (win && win->ghost_dragging) {
            win->ghost_x = g_mouse_x - win->drag_off_x;
            win->ghost_y = g_mouse_y - win->drag_off_y;
        }
        if(win&&win->resize_edges){
            int maxw=fb->width>3840?3840:fb->width;
            int maxh=(int)fb->height-g_title_h-g_taskbar_h;if(maxh>2160)maxh=2160;
            ResizeRect r=resize_rect(win->resize_original,win->resize_edges,g_mouse_x-win->resize_mouse_x,g_mouse_y-win->resize_mouse_y,
                win->min_w,win->min_h,maxw,maxh,g_title_h);
            win->ghost_x=r.x;win->ghost_y=r.y;win->ghost_w=r.w;win->ghost_h=r.h;
        }
    }

    if(mouse_release&&clipboard_press)clipboard_press=false;
    else if (mouse_release) {
        bool dragged = false;
        for (int i = 0; i < g_window_count; i++) {
            Window* win = g_window_list[i];
            if(win)win->restore_drag_pending=false;
            if(win&&win->resize_edges){wm_commit_resize(win);dragged=true;}
            if (win && win->ghost_dragging) {
                win->x = win->ghost_x;
                win->y = win->ghost_y;
                if (win->is_system && win->type == WIN_TYPE_TERM) {
                    g_term_ox = win->x;
                    g_term_oy = win->y;
                }
                win->ghost_dragging = false;
                dragged = true;
                g_dirty_min_x = 0; g_dirty_max_x = fb->width - 1;
                g_dirty_min_y = 0; g_dirty_max_y = fb->height - 1;
                g_needs_refresh = true;
            }
        }
        if (!dragged) {
            int32_t mx = g_mouse_x, my = g_mouse_y;
            int idx = wm_find_top_window(mx, my);
            if (idx != -1) {
                Window* win = g_window_list[idx];
                if (!wm_is_in_title(idx, mx, my) && !wm_is_in_close(idx, mx, my)) {
                    WinEvent ev;
                    ev.type = WIN_EVENT_MOUSE_UP;
                    wm_event_coordinates(win,mx,my,&ev);
                    ev.key = 1;
                    ev.mods = g_kbd_modifiers;
                    ev.delta = 0;
                    wm_win_push_event(win, ev);
                }
            }
        }
    }
    if((dx||dy)&&!g_clipboard_open&&!g_taskbar_popup){int idx=wm_find_top_window(g_mouse_x,g_mouse_y);
        if(idx>=0){Window* win=g_window_list[idx];
            if(!win->is_system&&!win->ghost_dragging&&!win->resize_edges&&!wm_is_in_title(idx,g_mouse_x,g_mouse_y)){
                WinEvent ev{};ev.type=WIN_EVENT_MOUSE_MOVE;wm_event_coordinates(win,g_mouse_x,g_mouse_y,&ev);
                ev.key=g_mouse_buttons;ev.mods=g_kbd_modifiers;wm_win_push_event(win,ev);}}}
}

void compositor_thread_main() {
    // Storage/root selection and the first wallpaper attempt precede the first
    // desktop frame. Keep animating the splash while boot makes that progress.
    while (!g_boot_assets_ready) {
        boot_splash_tick();
        task_sleep_ms(16);
    }
    // Keep the ring on screen throughout the expensive initial composition.
    // The normal dirty-rectangle presenter starts only after this full handoff.
    if (!fb || !g_master_backbuffer) {
        print((char*)"[boot] ERROR: No buffers for the initial desktop frame.");
        hcf();
    }
    // Read the RTC before the first frame so the taskbar clock never shows a placeholder.
    // The result is also written to the boot log, which is the quickest way to check the clock on hardware.
    {
        static char clk_line[48];
        if (rtc_poll()) {
            char tb[16], db[16];
            rtc_format_date(g_clock, db);
            rtc_format_time(g_clock, tb, true);
            char* p = clk_line;
            const char* s = "[clock] RTC OK: ";
            while (*s) *p++ = *s++;
            s = db; while (*s) *p++ = *s++;
            *p++ = ' ';
            s = tb; while (*s) *p++ = *s++;
            *p = 0;
            print(clk_line);
        } else {
            print((char*)"[clock] RTC read failed (no RTC, or invalid values). Tray shows --:--.");
        }
    }
#if BOOT_DIAGNOSTICS
    uint64_t first_frame_started = rdtsc();
#endif
    wm_compose_dirty(0, fb->width - 1, 0, fb->height - 1, false, false);
    if (!boot_splash_present_desktop(g_master_backbuffer)) hcf();
#if BOOT_DIAGNOSTICS
    char boot_elapsed[32];
    to_str((rdtsc() - first_frame_started) / (_usb_tsc_hz() / 1000), boot_elapsed);
    print((char*)"[boot] First desktop compose/present milliseconds:"); print(boot_elapsed);
    to_str((rdtsc() - g_boot_started_tsc) / (_usb_tsc_hz() / 1000), boot_elapsed);
    print((char*)"[boot] Kernel-to-desktop milliseconds:"); print(boot_elapsed);
#endif
    wm_overlay_update(g_mouse_x, g_mouse_y, g_mouse_buttons);
    g_desktop_ready = true;
    hda::desktop_ready();
#ifdef GPU_COMPOSITOR_TEST
    gpu_test_install();
#endif

    uint64_t target_tsc = rdtsc(); // Start cadence from current time
    uint64_t rainbow_epoch=target_tsc;
    static uint64_t last_hud_tick     = 0;
    static uint64_t last_clock_tick   = 0;
    uint64_t fps_sample_begin=target_tsc;
    uint64_t fps_presentations=0;

    while (true) {
        uint64_t frame_present_count=g_gpu_present_count;
        bool software_presented=false;
        if(GPU_RCS_ALPHA){
            g_gpu_timeline={};
            g_gpu_timeline.measure=(g_compositor_frame%16)==0;
            g_gpu_timeline.begin=rdtsc();
#if GPU_WORKLOAD
            g_gpu.workload=&g_gpu_timeline.workload;
#endif
            g_gpu_timeline.previous_latch=g_gpu_timeline_last_latch;
#if GPU_FRAME_DIAGNOSTICS
            if(g_gpu_timeline.previous_latch)g_gpu_timeline.display_previous=g_gpu_previous_phase;
            if(g_gpu_present_ready&&g_gpu_direct_verified)g_gpu_timeline.display_start=gpu_display_phase();
#endif
            g_gpu_timeline.cpu=wm_trace_cpu_cycles();
            g_gpu_timeline.bcs=g_gpu_perf.bcs_wait;g_gpu_timeline.rcs=g_gpu_perf.rcs_wait;
            g_gpu_timeline.sleep=g_gpu_perf.bcs_sleep.ticks+g_gpu_perf.rcs_sleep.ticks+g_gpu_perf.flip_sleep.ticks;
            g_gpu_timeline.bcs_batches=g_gpu_perf.bcs_batches;g_gpu_timeline.rcs_draws=g_gpu_perf.rcs_draws;
        }
#ifdef GPU_COMPOSITOR_TEST
        gpu_test_repair_scene();
#endif
        wm_collect_retired_surfaces();
        wm_titlebar_hover_damage();
        uint64_t frame_interval = (tsc_hz / 60);
        target_tsc += frame_interval;

        // Run one frame of compositor logic
        uint64_t now_refresh = rdtsc();
        g_rain_trace = {};
        
        // Keep the animation's phase, but spend no frame work on hidden pixels.
        Window* rain_win = nullptr;
        VisibleRegion rain_region{};
        for (int i = 0; i < g_window_count; i++) {
            if (g_window_list[i] && g_window_list[i]->visible && !g_window_list[i]->minimized && g_window_list[i]->type == WIN_TYPE_RAIN) {
                rain_win = g_window_list[i];
                rain_scroll = gpu_rainbow_phase(now_refresh-rainbow_epoch,tsc_hz);
                wm_visible_client(rain_win, &rain_region);
                if (GPU_RCS_ALPHA && rain_region.count) g_rain_trace.update = now_refresh;
                if (rain_region.count) g_needs_refresh = true;
                break;
            }
        }

        // Periodic HUD update: ensure Core0 stats refresh every 1 second
        if (now_refresh - last_hud_tick > tsc_hz) {
            last_hud_tick = now_refresh;
            g_hud_sample_due=true;
            TtyIrqGuard guard;
            if(wm_desktop_stats_visible()){
                // Composition suppresses both HUD lines under a maximized window.
                // Leave the sample due until the desktop is visible again.
                uint32_t hud_px=(uint32_t)HUD_MSG_CAP*8+32;
                uint32_t hud_left=fb->width>hud_px?fb->width-hud_px:0;
                wm_dirty_add(hud_left,fb->width-1,0,59);
                g_needs_refresh=true;
            }
        }

        // Taskbar clock: poll the RTC 4x per second (cheap: a handful of port reads) so a minute change shows
        // within ~250 ms. The tray is repainted only when the displayed text actually changed.
        if (now_refresh - last_clock_tick > tsc_hz / 4) {
            last_clock_tick = now_refresh;
            if (rtc_poll()) wm_clock_damage();
        }

        Window* cursor_window=wm_find_window(g_focused_win_id);
        term_cursor_tick(now_refresh/(tsc_hz/1000),tty_raw_mode&&g_term_view_offset==0&&
            cursor_window&&cursor_window->visible&&!cursor_window->minimized&&cursor_window->is_system&&cursor_window->type==WIN_TYPE_TERM&&
            wm_client_rect_exposed(cursor_window,g_term_ox+term_x*8,g_term_oy+term_y*10+8,8,2));
        bool fps_only_refresh=false;
        if(now_refresh-fps_sample_begin>=tsc_hz){
            uint64_t elapsed=now_refresh-fps_sample_begin;
            uint64_t rate=(fps_presentations*tsc_hz+elapsed/2)/elapsed;
            unsigned fps=rate>999?999:rate;
            fps_sample_begin=now_refresh;
            fps_presentations=0;
            if(fps!=g_desktop_fps){
                g_desktop_fps=fps;
                fps_only_refresh=!g_needs_refresh;
                wm_clock_damage();
            }
        }
        if (g_needs_refresh) {
            g_needs_refresh  = false;

            // Drain and render any pending terminal output
            term_process_ring_buffer();

            g_client_render_ticks = 0;
            bool retained_animation;
            {
#if GPU_FRAME_DIAGNOSTICS
                GpuCpuPhaseTimer cost(gpu_phase_cost(GpuFrameTimeline::Prepare),wm_trace_cpu_cycles);
#endif
                retained_animation=GPU_RCS_ALPHA&&g_gpu_direct_verified&&g_gpu_present_ready&&
                    wm_prepare_retained_animation(rain_win);
            }
            wm_rain_render(rain_win, rain_region,retained_animation);

            __asm__ volatile("cli");
			uint32_t fx1 = g_dirty_min_x, fx2 = g_dirty_max_x;
			uint32_t fy1 = g_dirty_min_y, fy2 = g_dirty_max_y;
			g_dirty_min_y = fb->height; g_dirty_max_y = 0;
			g_dirty_min_x = fb->width;  g_dirty_max_x = 0;
			// Take the rect list atomically with the legacy box. Copied field by field on purpose: a struct
			// assignment makes the compiler emit a call to an external memcpy, which this kernel does not export.
			DamageList frame_dmg;
			frame_dmg.n = g_damage.n;
			for (uint32_t di = 0; di < g_damage.n; di++) {
				frame_dmg.r[di].x1 = g_damage.r[di].x1; frame_dmg.r[di].y1 = g_damage.r[di].y1;
				frame_dmg.r[di].x2 = g_damage.r[di].x2; frame_dmg.r[di].y2 = g_damage.r[di].y2;
			}
			damage_clear(&g_damage);
			__asm__ volatile("sti");

			// Compose each damaged region separately (HUD strip, animation, cursor ... are not
			// merged into one giant box). The legacy box counts as one more rect when something wrote to it.
			uint64_t t_start = rdtsc();
            if(GPU_RCS_ALPHA)g_gpu_timeline.compose=t_start;
            gpu_trace_stage("COMPOSE");
			if (!g_pause_compositor) {
                damage_add_clipped(&frame_dmg,fb->width,fb->height,fx1,fx2,fy1,fy2);
                bool retained_prepared=retained_animation;
                auto add_animation_damage=[&](){
                    VisibleRect bounds;
                    if(gpu_animation_damage_bounds(rain_region,&bounds))
                        damage_add_clipped(&frame_dmg,fb->width,fb->height,bounds.x1,bounds.x2-1,bounds.y1,bounds.y2-1);
                };
                if(retained_prepared&&!retained_animation)add_animation_damage();
                static bool master_needs_repaint=false;
                static VisibleRegion exact_damage{};
                bool direct;
                {
#if GPU_FRAME_DIAGNOSTICS
                    GpuCpuPhaseTimer cost(gpu_phase_cost(GpuFrameTimeline::Direct),wm_trace_cpu_cycles);
#endif
                    direct=begin_direct_scanout(frame_dmg,GPU_EXACT_REPAIRS?&exact_damage:nullptr,retained_animation);
                }
                if(retained_animation&&!direct){retained_animation=false;add_animation_damage();}
                if(!direct && master_needs_repaint){
                    frame_dmg.n=1;
                    frame_dmg.r[0]={0,0,(uint32_t)fb->width-1,(uint32_t)fb->height-1};
                }
                master_needs_repaint=direct;
                static VisibleRegion retained_repairs{};
                bool repairs_ok=true;
                if(retained_animation){
#if GPU_FRAME_DIAGNOSTICS
                    GpuCpuPhaseTimer cost(gpu_phase_cost(GpuFrameTimeline::RepairPlan),wm_trace_cpu_cycles);
#endif
                    if(GPU_EXACT_REPAIRS)repairs_ok=gpu_retained_repair_region(&retained_repairs,exact_damage,g_retained_animation_plan);
                    else repairs_ok=gpu_retained_repair_region(&retained_repairs,frame_dmg,g_retained_animation_plan);
                }
                if(retained_animation&&!repairs_ok){
                    retained_animation=false;add_animation_damage();
                }
                if(retained_animation&&GPU_EXACT_REPAIRS){
                    ++g_gpu_perf.exact_repair_frames;
                    for(unsigned i=0;i<retained_repairs.count;++i){auto r=retained_repairs.rects[i];
                        g_gpu_perf.exact_repair_pixels+=(uint64_t)(r.x2-r.x1)*(r.y2-r.y1);}
                }
                uint32_t total = retained_animation?retained_repairs.count:frame_dmg.n;
#if GPU_ALPHA_REPAIR_BATCH
                wm_begin_alpha_repairs(retained_animation);
#endif
				bool overlay_done = false;
				uint64_t a_bg = 0, a_win = 0, a_rain = 0, a_lock = 0, a_vram = 0;
				if (total == 0 && !retained_animation) {
                    // Pointer/outline motion changes only the VRAM overlay.
                    // Never enter scene composition with the empty damage box.
                    uint64_t overlay_started=rdtsc();
                    wm_overlay_update(g_mouse_x,g_mouse_y,g_mouse_buttons);
                    overlay_done=true;a_vram=rdtsc()-overlay_started;
				} else {
#if GPU_FRAME_DIAGNOSTICS
                    GpuCpuPhaseTimer cost(gpu_phase_cost(GpuFrameTimeline::RepairPaint),wm_trace_cpu_cycles);
#endif
                    if(retained_animation){
                        wm_compose_retained_repairs(retained_repairs);
                        a_bg+=g_time_bg_ticks;a_win+=g_time_win_ticks;a_rain+=g_time_rain_ticks;
                        a_lock+=g_time_lock_ticks;a_vram+=g_time_vram_ticks;
                    }else for(unsigned i=0;i<total;++i){
                        const auto r=frame_dmg.r[i];
                        if(wm_compose_dirty(r.x1,r.x2,r.y1,r.y2,i+1==total))overlay_done=true;
                        a_bg+=g_time_bg_ticks;a_win+=g_time_win_ticks;a_rain+=g_time_rain_ticks;
                        a_lock+=g_time_lock_ticks;a_vram+=g_time_vram_ticks;
                    }
                }
#if GPU_ALPHA_REPAIR_BATCH
                g_deferred_alpha.active=false;
#endif
                if(retained_animation){
                    uint64_t animation_started=rdtsc();
                    // Repairs can block while input changes the window stack.
                    // Refresh source snapshots, and rebuild the complete scene
                    // if geometry/opacity changed since the animation snapshot.
                    uint64_t layout=g_retained_animation_layout;
                    uint64_t repair_menu_key=g_retained_menu_key;
                    bool stable,prepared;
                    {
#if GPU_FRAME_DIAGNOSTICS
                        GpuCpuPhaseTimer cost(gpu_phase_cost(GpuFrameTimeline::Revalidate),wm_trace_cpu_cycles);
#endif
                        prepared=gpu_direct_active()&&wm_prepare_retained_animation(rain_win);
                        stable=prepared&&layout==g_retained_animation_layout;
                    }
#if GPU_ALPHA_REPAIR_BATCH
                    stable=stable&&wm_alpha_repairs_ready();
                    if(!stable)wm_cancel_alpha_repairs();
#endif
                    // One layout transition can be repaired against the fresh
                    // animation plan instead of drawing that animation again in
                    // a full-scene fallback. A second change keeps the old fallback.
                    if(!stable&&prepared&&gpu_direct_active()&&layout!=g_retained_animation_layout){
                        static VisibleRegion all{},rebased{};
                        all.count=1;all.rects[0]={0,0,(int)fb->width,(int)fb->height};
                        if(gpu_retained_repair_region(&rebased,all,g_retained_animation_plan)){
                            ++g_gpu_perf.transition_rebases;
                            layout=g_retained_animation_layout;repair_menu_key=g_retained_menu_key;
#if GPU_ALPHA_REPAIR_BATCH
                            wm_begin_alpha_repairs(true);
#endif
                            {
#if GPU_FRAME_DIAGNOSTICS
                                GpuCpuPhaseTimer cost(gpu_phase_cost(GpuFrameTimeline::RepairPaint),wm_trace_cpu_cycles);
#endif
                                wm_compose_retained_repairs(rebased);
                            }
#if GPU_ALPHA_REPAIR_BATCH
                            g_deferred_alpha.active=false;
#endif
                            {
#if GPU_FRAME_DIAGNOSTICS
                                GpuCpuPhaseTimer cost(gpu_phase_cost(GpuFrameTimeline::Revalidate),wm_trace_cpu_cycles);
#endif
                                stable=gpu_direct_active()&&wm_prepare_retained_animation(rain_win)&&
                                    layout==g_retained_animation_layout;
                            }
#if GPU_ALPHA_REPAIR_BATCH
                            stable=stable&&wm_alpha_repairs_ready();
                            if(!stable)wm_cancel_alpha_repairs();
#endif
                            if(!stable)++g_gpu_perf.transition_rebase_failed;
                        }
                    }
                    bool drawn=false;
                    if(stable)drawn=wm_draw_retained_animation(repair_menu_key);
                    else {
#if GPU_FRAME_DIAGNOSTICS
                        GpuCpuPhaseTimer cost(gpu_phase_cost(GpuFrameTimeline::Fallback),wm_trace_cpu_cycles);
#endif
                        retained_animation=false;
                        overlay_done=wm_compose_dirty(0,fb->width-1,0,fb->height-1,true);
                    }
                    if(stable&&drawn&&g_retained_scene_invalidated){
#if GPU_FRAME_DIAGNOSTICS
                        GpuCpuPhaseTimer cost(gpu_phase_cost(GpuFrameTimeline::Fallback),wm_trace_cpu_cycles);
#endif
#if GPU_ALPHA_REPAIR_BATCH
                        wm_cancel_alpha_repairs();
#endif
                        retained_animation=false;
                        overlay_done=wm_compose_dirty(0,fb->width-1,0,fb->height-1,true);
                    }
                    a_win+=rdtsc()-animation_started;
                    uint64_t present_started=rdtsc();
                    if(retained_animation&&drawn){
                        wm_overlay_update(g_mouse_x,g_mouse_y,g_mouse_buttons);
                        overlay_done=g_gpu_present_hook!=nullptr;
                    }
                    if(retained_animation&&(!drawn||!overlay_done)){
#if GPU_FRAME_DIAGNOSTICS
                        GpuCpuPhaseTimer cost(gpu_phase_cost(GpuFrameTimeline::Fallback),wm_trace_cpu_cycles);
#endif
#if GPU_ALPHA_REPAIR_BATCH
                        wm_cancel_alpha_repairs();
#endif
                        if(g_gpu_present_ready)gpu_scene_abort("GPU: retained animation presentation failed");
                        g_rain_surface_current=false;
                        overlay_done=wm_compose_dirty(0,fb->width-1,0,fb->height-1,true);
                        master_needs_repaint=false;
                    }
                    a_vram+=rdtsc()-present_started;
                }
                gpu_direct_end();
				// Safety net for frames with no completed scene blit (for example, pointer-only motion).
				if (!overlay_done) wm_overlay_update(g_mouse_x, g_mouse_y, g_mouse_buttons);
                software_presented=total&&overlay_done&&!g_gpu_present_hook;
				// HUD shows the whole frame, not just the last rect.
				g_snap_bg = a_bg; g_snap_win = a_win; g_snap_rain = a_rain + g_client_render_ticks;
				g_snap_lock = a_lock; g_snap_vram = a_vram;
					g_gpu_perf.scene_ticks+=a_bg+a_win+a_lock+a_vram;
					g_gpu_perf.present_ticks+=a_vram;
			}
			uint64_t t_end = rdtsc();
			g_last_frame_ticks = t_end - t_start;

            term_dirty_reset();
        }

        if(!fps_only_refresh&&(software_presented||g_gpu_present_count!=frame_present_count))
            ++fps_presentations;
        if(GPU_RCS_ALPHA)gpu_timeline_finish(rdtsc(),wm_trace_cpu_cycles(),g_gpu_perf);
#if GPU_FRAME_DIAGNOSTICS
        if(GPU_RCS_ALPHA)log_frame_gap(g_gpu_timeline);
#endif
#if GPU_WORKLOAD
        g_gpu.workload=nullptr;
#endif
        // A confirmed synchronous flip already paced this frame to the display.
        // Starting its successor immediately avoids adding a timer/yield delay
        // after vblank. GPU waits still block and let other tasks run.
        // Software presentation and idle loops retain the timed cadence below.
        if(GPU_WORKLOAD || GPU_FRAME_DIAGNOSTICS){
            const uint64_t sample_now=rdtsc();
            wm_gpu_perf_sample(sample_now);
            log_app_launches(sample_now);
            log_menu_opens(sample_now);
        }
        uint64_t final_now = rdtsc();
        if(g_gpu_present_count!=frame_present_count){
            target_tsc=final_now;
        } else if (final_now < target_tsc) {
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
    g_boot_started_tsc = rdtsc();
	if (!LIMINE_BASE_REVISION_SUPPORTED) hcf();
	if (!fb_req.response || fb_req.response->framebuffer_count < 1) hcf();

	// Keep all rendering disabled until we own a valid framebuffer mapping.
    // Some firmware/loader modes provide its HHDM address without mapping the
    // aperture; drawing the splash first faults before an IDT exists.
    struct limine_framebuffer* boot_framebuffer=fb_req.response->framebuffers[0];

    // --- SSE & SIMD Enablement (CR0, CR4) ---
    {
        uint64_t cr0;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        cr0 &= ~((1ULL << 2) | (1ULL << 3)); // Clear EM and TS (eager x87/SSE use)
        cr0 |= (1ULL << 1);  // Set MP (Monitor Coprocessor)
        __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");

        uint64_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1ULL << 9);  // Set OSFXSR (FXSAVE/FXRSTOR support)
        cr4 |= (1ULL << 10); // Set OSXMMEXCPT (SIMD exception support)
        __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
        uint32_t mxcsr=0x1f80;
        __asm__ volatile("fninit; ldmxcsr %0" :: "m"(mxcsr) : "memory");
    }

	if (!boot_framebuffer || !boot_framebuffer->address || boot_framebuffer->bpp != 32 ||
        !boot_framebuffer->width || !boot_framebuffer->height ||
        boot_framebuffer->width>UINT64_MAX/4 || boot_framebuffer->pitch<boot_framebuffer->width*4 ||
        boot_framebuffer->pitch>UINT64_MAX/boot_framebuffer->height) hcf();
	print((char*)"[boot] Kernel entered. Limine handoff complete (BSP only).");

	// ── NEW ──────────────────────────────────────
	init_gdt();
	syscall_init();
	print((char*)"GDT loaded.");
	init_idt();
	init_cpu_power();
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
	print((char*)"[boot] Mapping usable RAM regions...");
	map_hhdm_usable(PRESENT | WRITABLE);
	print((char*)"[boot] Usable RAM mapped.");
	setup_pat();
	
	// Explicitly map framebuffer pages before the first splash/log write. Include
    // both boundary pages if firmware returns a framebuffer with a byte offset.
	{
        uint64_t address=(uint64_t)boot_framebuffer->address;
        uint64_t base=address&~(PAGE_SIZE-1);
        if (base<HHDM) hcf();
		uint64_t phys=base-HHDM;
		uint64_t size=boot_framebuffer->pitch*boot_framebuffer->height;
        if (size>UINT64_MAX-(address-base) || base>UINT64_MAX-size-(address-base)) hcf();
        size+=address-base;
		// Map it into the HHDM range explicitly with Write-Combining (WC) enabled via PCD bit.
		// PCD (bit 4) selects PAT entry that is typically WC on modern x86.
		uint64_t flags = PRESENT | WRITABLE | (1ULL << 4); 
		for (uint64_t offset = 0; offset < size; offset += 4096) {
			map_page(base + offset, phys + offset, flags);
		}
	}
    fb=boot_framebuffer;
    boot_splash_begin();
    print((char*)"[boot] Framebuffer mapped before first draw.");
    print_fb_pixel_format();

	// LAPIC/HPET fallback needs MMIO mappings, so start the timer after PMM/VMM.
	init_boot_timer();
	print((char*)"[boot] Initializing PS/2 input...");
	synaptics_init();
	ps2kb_init();
	print((char*)"Interrupts enabled. Timer running.");
	init_heap();
    
    // Allocate backbuffer for double buffering + huge scrollback history
    if (fb_req.response && fb_req.response->framebuffer_count > 0) {
        fb = fb_req.response->framebuffers[0];
        
        // Userspace terminal surfaces are allocated through the window API.

        // Page-aligned: the GPU maps this buffer page-by-page into the GGTT and the PRM
        // requires 64-byte-aligned linear surface bases. kmalloc only guarantees 16 bytes.
        {
            uint8_t* mb_raw = (uint8_t*)kmalloc((uint64_t)fb->height * fb->pitch + 4096);
            g_master_backbuffer = mb_raw ? (uint8_t*)(((uint64_t)mb_raw + 4095) & ~4095ULL) : nullptr;
        }
        if (g_master_backbuffer) {
            memset(g_master_backbuffer, 0, fb->height * fb->pitch);
            
            g_needs_refresh = true;
        }
    }

	ramfs_init();
	ramfs_mount_klog();
    vnode* recovery_root = vfs_root;
    vnode* live_log = vfs_namei("/bootlog.txt");
    vfs_mkdir("/tmp");
    vfs_mkdir("/run");
    vnode* runtime_tmp = vfs_namei("/tmp");
    vnode* runtime_run = vfs_namei("/run");
    file runtime_log{};
    if (vfs_open("/run/boot.log", 0x41, &runtime_log) == 0) {
        runtime_log.vn->ops = &klog_vfs_ops;
        vfs_close(&runtime_log);
    }
    static bool storage_selected = false;
    static bool disk_root_active = false;
    limine_file* recovery_archive = nullptr;

    // Check for initrd tar archive in Limine modules and unpack into ramfs
    if (module_req.response) {
        for (uint64_t m = 0; m < module_req.response->module_count; m++) {
            struct limine_file* mod = module_req.response->modules[m];
            if (mod && mod->size >= 512) {
                const char* magic = (const char*)mod->address + 257;
                if (magic[0] == 'u' && magic[1] == 's' && magic[2] == 't' && magic[3] == 'a' && magic[4] == 'r') {
                    tar_unpack((const uint8_t*)mod->address, mod->size);
                    recovery_archive=mod;
                    break;
                }
            }
        }
    }


    scheduler_init();
    // Initializing graphical UI components
#if BOOT_DIAGNOSTICS
    uint64_t ui_started = rdtsc();
#endif
    wm_init();     // Initialize window manager and UI scaling first
    g_audio_mixer.changed=wm_audio_changed;
    wm_prepare_menu_cache();
    g_task_exit_hook = process_exit_ui;
    g_stdio_exit_hook = console_release;
    wm_log_init(); // Then initialize the log buffer with the correct scaled dimensions
#if BOOT_DIAGNOSTICS
    char ui_elapsed[32];
    to_str((rdtsc() - ui_started) / (_usb_tsc_hz() / 1000), ui_elapsed);
    print((char*)"[boot] UI initialization milliseconds:"); print(ui_elapsed);
#endif
    
    // Spawn compositor thread extremely early so it guarantees 60fps
    // screen refreshes regardless of whether USB enumeration blocks.
    scheduler_add_task(task_create(compositor_thread_main,16,"Compositor"));
    scheduler_add_task(task_create(hda::service_task,16,"Audio service"));



    print((char*)"[boot] Discovering PCI through ACPI (XSDT/RSDT)...");
    volatile struct MCFGHeader *mcfg = (volatile struct MCFGHeader*)find_acpi_table("MCFG");
    if (!mcfg || mcfg->header.length < sizeof(struct ACPISDTHeader) + 8) {
        print((char*)"[boot] ERROR: No valid ACPI MCFG table.");
        hcf();
    }
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
#if GPU_RENDER && GPU_TRACE
        memset(trace_bus_parent,0,sizeof(trace_bus_parent));
#endif
        find_intel_gpu(virt_base, start, start, end);
        if (g_igpu.found) map_intel_gpu_mmio();
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
                    hda::discover(virt_base, seg, start, bus, dev, fn, id, class_reg);
#if GPU_RENDER && GPU_TRACE
                    trace_pci_discover({virt_base,id,seg,start,(uint8_t)bus,dev,fn},class_reg);
#endif
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
#if GPU_RENDER && GPU_TRACE
    trace_nvidia_power("GPU PCI power snapshot: boot");
#endif
	if (g_igpu.found) {
		// Full bring-up + self-test (fill, copy, 64x backbuffer flip). Never halts: on any
		// failure the compositor simply keeps using the CPU copy path.
#if GPU_BRINGUP
		if (gpu_bringup(g_rainbow_wide)) {
            if(g_gpu_compose_ready)g_gpu_compose_hook=gpu_compose_scene;
            if(GPU_RENDER && g_gpu_compose_ready && gpu_render_initialize())g_gpu_render_hook=gpu_render_scene;
            if(g_gpu_present_ready)g_gpu_present_hook=gpu_present_frame;
            else if(!GPU_COMPOSE && g_gpu_flip_preferred)g_gpu_flip_hook=gpu_flip_rect;
        }
#else
		gpu_status_set("GPU:off");   // found, but not initialised (see Makefile: GPU=1)
#endif
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
#if BOOT_DIAGNOSTICS
	uint64_t usb_scan_started = _usb_rdtsc();
#endif
	bool usb_restarted[MAX_CONTROLLERS] = {};
	// Serialize controller resets against synchronous filesystem I/O. Existing
	// mounted disks are offlined by setupUSB; never rebind an open vnode.
    // Discovery and hotplug can wait hundreds of milliseconds. The storage
    // mutex owns their rings; keep the scheduler/compositor timer running.
	__asm__ volatile("sti" ::: "memory");
    {
    USBTransactionGuard storage_serialization;
    for (int c = 0; c < xhci_count; c++) {
        if (xhci_hc[c].portfailed) {
            for (int _p = 0; _p < 4096; _p++) xhci_hc[c].portfailed[_p] = 0;
        }
    }
    
        for (int c = 0; c < xhci_count; c++) {
            if (xhci_hc[c].needsResetting && !xhci_hc[c].unavailable) {
                usb_restarted[c] = true;
                setupUSB(&xhci_hc[c]);
                if (xhci_hc[c].needsResetting) {
                    print((char*)"[usb] Controller initialization failed; continuing with other controllers.");
                    xhci_hc[c].unavailable = true;
                }
            }
        }

#if BOOT_DIAGNOSTICS
    to_str((_usb_rdtsc() - usb_scan_started) / (_usb_tsc_hz() / 1000), str);
    print((char*)"[boot] USB discovery milliseconds:"); print(str);
#endif

    // Select once before launching userspace; never switch roots after unplug/replug.
    if (!storage_selected) {
        usb_wait_for_boot_storage();
        storage_selected = true;
        vnode* candidate = nullptr;
        static wy_ext4_volume ext4_candidate;
        unsigned candidates = 0;
        const char* recovery_reason = "No USB storage device discovered before root selection.";
        for (int d = 0; d < usb_disk_count; ++d) {
            if (!usb_disks[d].disk.online) continue;
            wy_ext4_volume probe{};
            unsigned ext4_count = wy_ext4_probe(&usb_disks[d].disk, &probe);
            if (ext4_count) {
                candidates += ext4_count;
                ext4_candidate = probe;
            }
        }
        if (!candidates) {
            unsigned online = 0;
            for (int d = 0; d < usb_disk_count; ++d) if (usb_disks[d].disk.online) ++online;
            if (online) recovery_reason = "USB disk online, but no readable WYZARDATA partition was recognized.";
            else if (usb_disk_count) recovery_reason = "USB disk was discovered, but was offline before root selection.";
            print((char*)"[storage] No readable WYZARDATA system partition found.");
        } else if (candidates > 1) {
            recovery_reason = "Multiple system volumes found; root selection is ambiguous.";
            print((char*)"[storage] Multiple system volumes found; refusing ambiguous root selection.");
        }
        if (candidates == 1) {
            int setup = wy_usb_autogrow(&ext4_candidate, storage_setup_progress);
            if (setup >= 0) candidate = wy_ext4_mount(&ext4_candidate);
            recovery_reason = candidate ? "System-root marker /etc/root.cfg could not be opened." :
                (setup < 0 ? "Automatic storage setup failed; seed preserved. Reboot to retry." :
                "System partition recognized, but ext4 mount/journal initialization failed.");
            if (candidate) vfs_mount_root(candidate);
            file marker{};
            char signature[12] = {};
            bool valid = candidate && vfs_open("/etc/root.cfg", 0, &marker) == 0;
            if (valid) {
                uint64_t read_bytes=vfs_read(&marker, signature, 11);
                recovery_reason=read_bytes==11 ?
                    "System-root marker /etc/root.cfg has an invalid signature." :
                    "System-root marker /etc/root.cfg returned a short or failed read.";
                valid = read_bytes == 11 && strncmp(signature, "WYZARROOT1\n", 11) == 0;
                if(!valid){
                    char count_text[32];
                    print((char*)"[storage] Root marker file size:");
                    to_str(marker.vn->size,count_text);print(count_text);
                    print((char*)"[storage] Root marker bytes read:");
                    to_str(read_bytes,count_text);print(count_text);
                    char hex[23]{};
                    const char* digits="0123456789ABCDEF";
                    for(unsigned i=0;i<read_bytes && i<11;++i){
                        unsigned byte=(uint8_t)signature[i];
                        hex[i*2]=digits[byte>>4];hex[i*2+1]=digits[byte&15];
                    }
                    print((char*)"[storage] Root marker bytes (hex):");
                    print(hex);
                }
                vfs_close(&marker);
            }
            vnode* shell = vfs_namei("/bin/init.elf");
            if (valid && (!shell || shell->type != VnodeType::FILE || !shell->size))
                recovery_reason = "System-root signature valid, but /bin/init.elf is missing or empty.";
            valid = valid && shell && shell->type == VnodeType::FILE && shell->size > 0;
            if (valid) recovery_reason = "System root validated, but runtime mounts could not be attached.";
            vnode* tmp_point = vfs_namei("/tmp");
            vnode* run_point = vfs_namei("/run");
            vnode* log_point = vfs_namei("/bootlog.txt");
            if (valid && vfs_mount(tmp_point, runtime_tmp) == 0 &&
                vfs_mount(run_point, runtime_run) == 0 && vfs_mount(log_point, live_log) == 0 &&
                vfs_mount(vfs_namei("/run/boot.log"), live_log) == 0) {
                disk_root_active = true;
                print((char*)"[storage] WYZARDATA mounted as root /.");
                print((char*)"[storage] ext4 journal active on USB system root.");
            } else {
                print((char*)"[storage] System signature, init file or runtime mounts unavailable.");
                vfs_mount_root(recovery_root);
                if (ext4_candidate.mounted) wy_ext4_unmount(&ext4_candidate);
            }
        }
        if (!disk_root_active) {
            print((char*)"[storage] No unique valid system volume; RAM recovery mode.");
            usb_storage_failure_summary();
            storage_record_recovery(runtime_run, recovery_reason);
        }
    }
    }
	__asm__ volatile("sti" ::: "memory");
    static bool wallpaper_loaded = false;
    if (!wallpaper_loaded) {
        wallpaper_loaded = true;
#if BOOT_DIAGNOSTICS
        uint64_t font_started = rdtsc();
#endif
        wm_load_disk_font();
#if BOOT_DIAGNOSTICS
        char asset_elapsed[32];
        to_str((rdtsc() - font_started) / (_usb_tsc_hz() / 1000), asset_elapsed);
        print((char*)"[font] Load/bake milliseconds:"); print(asset_elapsed);
#endif
        if (disk_root_active) wm_load_disk_wallpaper();
        else print((char*)"[wallpaper] RAM recovery root active; using generated wallpaper.");
        g_resources_requested=false;
        wm_resources_reload();
        scheduler_add_task(task_create(wm_resource_worker,16,"Desktop resources"));
        desktop_launcher_task=task_create(desktop_launch_worker,16,"Desktop launcher");
        scheduler_add_task(desktop_launcher_task);
        g_desktop_shortcut_hook=desktop_shortcut;
        if (!g_wallpaper) {
#if BOOT_DIAGNOSTICS
            uint64_t fallback_started = rdtsc();
#endif
            wm_init_wallpaper();
#if BOOT_DIAGNOSTICS
            to_str((rdtsc() - fallback_started) / (_usb_tsc_hz() / 1000), asset_elapsed);
            print((char*)"[wallpaper] Fallback generation milliseconds:"); print(asset_elapsed);
#endif
        }
        g_boot_assets_ready = true;
        boot_loading_screen_until_ready();
    }

	// Discard enumeration leftovers before arming ANY new input/status pipe.
	// Unchanged controllers keep both their event queues and armed transfers.
	for (int c = 0; c < xhci_count; ++c) {
		if (!usb_restarted[c]) continue;
		USB_Response stale;
		if (xhci_hc[c].unavailable) continue;
		for (uint32_t drained = 0; drained < ER_RING_SIZE * 2; ++drained) {
			stale = get_usb_response(&xhci_hc[c], 1);
			if (!stale.gotresponse) break;
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
		if (hub_watch[h].armed || !hub_watch[h].ready || hub_watch[h].pipe_failed) continue;

		hub_queue_status(h);
		print((char*)"Queued status-change TRB for hub slot:");
		to_str(hd->slot_id, str); print(str);
	}

	print((char*)"[init] Decoupled terminal boot.");
	static bool init_started = false;
	if (!init_started) {
		Task* init_task = execve_path("/bin/init.elf");
        Task* console_host=exec_console_host("/bin/init.elf");
        if ((!init_task || !console_host) && disk_root_active) {
            if(init_task){task_free(init_task);init_task=nullptr;}
            if(console_host){console_release(console_host);task_free(console_host);console_host=nullptr;}
            // Failed disk shell: enter the complete RAM recovery environment.
            vfs_mount_root(recovery_root);
            disk_root_active = false;
            g_resources_refresh=true; g_resources_requested=true; ++g_resource_demand_generation;
            print((char*)"[storage] Disk terminal failed; RAM recovery mode.");
            storage_record_recovery(runtime_run, "System partition mounted, but loading the terminal or shell failed.");
            init_task = execve_path("/bin/init.elf");
            console_host=exec_console_host("/bin/init.elf");
        }
        if (!init_task && module_req.response) {
            for (uint64_t m = 0; m < module_req.response->module_count; ++m) {
                struct limine_file* mod = module_req.response->modules[m];
                const char* path = mod ? mod->path : nullptr;
                if (!path) continue;
                const char* base = path;
                for (const char* p = path; *p; ++p) if (*p == '/') base = p + 1;
                if (strcmp(base, "init.elf") == 0 && mod->size >= sizeof(Elf64_Ehdr)) {
                    init_task = execve_memory((const uint8_t*)mod->address);
                    break;
                }
            }
        }
		if (init_task && console_host) {
			g_terminal_shell_ready=false;
			g_terminal_shell_pid = init_task->pid;
            auto* console=(AppConsole*)console_host->console;
            ++console->refs;
            console->root_pid=console->foreground=init_task->pid;
            init_task->console=console;
            init_task->parent_pid=console_host->pid;
            console_host->gui_app=true;
            console_host->focus_first_window=true;
			// Phase 9: register post-flip chrome hook
			g_pre_flip_hook = nullptr; // Phase 18: handled by wm_compose_full
			g_post_flip_hook = nullptr;

			// Re-enable the shell task — it now runs "inside" the WM window
			scheduler_add_task(init_task);
            scheduler_add_task(console_host);
			init_started = true;
			g_klog_bypass_framebuffer = true;
			print((char*)"[wm] Shell task started inside WM frame.");
		} else {
            if(init_task)task_free(init_task);
            if(console_host){console_release(console_host);task_free(console_host);}
			print((char*)"[init] Failed to load terminal or shell task.");
		}
	}

    static bool external_storage_started = false;
    if (!external_storage_started) {
        external_storage_started = true;
        scheduler_add_task(task_create(fat_mount_worker,16,"External drives"));
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
	// tsc_hz is calibrated by the selected boot timer earlier in kmain.

	// Print so you can verify CPUID gave a sensible value.
	print((char*)"TSC freq (Hz):"); to_str(tsc_hz, str); print(str);

	const uint64_t REPEAT_DELAY  = tsc_hz * 250 / 1000;   // 250 ms in ticks
	const uint64_t REPEAT_PERIOD = tsc_hz *  40 / 1000;   //  40 ms in ticks

	// ── Per-keyboard state ────────────────────────────────────────────────────
	auto media_key=[](unsigned key){TtyIrqGuard guard;++g_usb_media_key_events;g_audio_hotkeys.queue(key);};

	uint8_t  last_keys[MAX_KEYBOARDS][6];
	uint8_t  last_mods[MAX_KEYBOARDS];
	WinKeyTap win_taps[MAX_KEYBOARDS]{};
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

	for (int k = 0; k < keyboard_count; k++) {
		USBDevice* kd = keyboards[k];
		for (int i = 0; i < kd->interface_count; i++) {
			HIDInterface* hi = &kd->interfaces[i];
			if ((hi->type != 1 && hi->type != 4) || hi->armed || !hi->configured) continue;

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
			hi->armed = true;
			kd->hc->doorbell32[kd->slot_id] = kbd_dci;
		}
	}

	// ── Queue one initial TRB per mouse ──────────────────────────────────────
	for (int m = 0; m < mouse_count; m++) {
		USBDevice* md = mice[m];
		for (int i = 0; i < md->interface_count; i++) {
			HIDInterface* hi = &md->interfaces[i];
			if (hi->type != 2 || hi->armed || !hi->configured) continue;

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
			hi->armed = true;
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

		// Bound each batch so a noisy port cannot starve hotplug, input repeat
		// and desktop work indefinitely. Process every controller each round.
        { KernelCpuScope cpu_scope(KCPU_USB_EVENTS);
		while (event_count < ER_RING_SIZE) {
            bool any_resp = false;
            for (int ctrl_idx = 0; ctrl_idx < xhci_count; ctrl_idx++) {
                USBControlGuard event_ownership;
                // The synchronous owner polls and defers unrelated events.
                // Do not steal its command/EP0 completion while it is asleep.
                if (usb_control_owner && usb_control_owner != g_current_task) continue;
                if (xhci_hc[ctrl_idx].needsResetting) continue;
			    // 0ms timeout: fast-poll the event ring without busy-waiting.
			    USB_Response resp = get_usb_response(&xhci_hc[ctrl_idx], 0);
			    if (!resp.gotresponse) continue;

                any_resp = true;
			    event_count++;

                if (usb_storage_dispatch_completion(&xhci_hc[ctrl_idx], resp)) continue;
                if (hub_dispatch_control(&xhci_hc[ctrl_idx], resp)) continue;

			// ── Port Status Change (type 34) ─────────────────────────────────
			// Reset completion and link power changes also generate PSC. Acknowledge
			// them here; the nonblocking sampler decides if presence changed.
			if (resp.type == 34) {
				uint32_t port_id = (resp.event->parameter >> 24) & 0xFF;
				XHCIController* hc = &xhci_hc[ctrl_idx];
				if (port_id && port_id <= hc->max_ports) {
					volatile uint32_t* portsc = (volatile uint32_t*)((uintptr_t)hc->ops + 0x400 + (port_id - 1) * 0x10);
					uint32_t sc = *portsc;
					usb_root_ack_changes(hc, port_id - 1, sc, _usb_rdtsc());
				}
				continue;
			}

			if (resp.type == 32) {   // Transfer Event
				uint32_t code    = (resp.event->status >> 24) & 0xFF;
				uint64_t trb_ptr = resp.event->parameter;
				uint32_t slot_from_event = (resp.ctrl >> 24) & 0xFF;

				// ── Hub status-change interrupt ───────────────────────────────
				//
				// Notifications only schedule status reads. Latched reset/enable
				// changes must be cleared, not treated as a controller failure.
				{
					int which_hub = -1;
					for (int h = 0; h < hub_count; h++) {
						if (hubs[h]->slot_id == slot_from_event && hubs[h]->hc == &xhci_hc[ctrl_idx] &&
						    ((resp.ctrl >> 16) & 31) == hub_dcis[h] &&
						    trb_ptr >= hubs[h]->hub_ring->phys &&
						    trb_ptr < hubs[h]->hub_ring->phys + LINK_INDEX * 16u) { which_hub = h; break; }
					}

					if (which_hub >= 0 && (code == 1 || code == 13)) {
						if (!hub_watch[which_hub].saw_notification) {
							print((char*)"[usb] Hub notification received; checking port status.");
							hub_watch[which_hub].saw_notification = true;
						}
						hub_watch[which_hub].armed = false;
						hub_note_report(which_hub);
						// Rearm only after latched changes are acknowledged; otherwise
						// an interrupt storm can keep us in this drain loop forever.
						continue;
					}

					// A failed interrupt pipe does not establish that the hub or its
					// children disconnected. Poll EP0; root presence handles removal.
					if (which_hub >= 0 && code != 1 && code != 13) {
						print((char*)"Hub transfer error, code:");
						to_str(code, str); print(str);
						hub_watch[which_hub].pipe_failed = true;
						hub_watch[which_hub].armed = false;
						hub_watch[which_hub].poll.pending |= (1u << (hub_watch[which_hub].num_ports + 1)) - 1u;
						hub_watch[which_hub].poll.pending_after = 0;
						continue;
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
								if ((keyboards[k]->interfaces[i].type == 1 || keyboards[k]->interfaces[i].type == 4) &&
									(keyboards[k]->interfaces[i].ep_num*2+1) == dci_from_event &&
									trb_ptr >= keyboards[k]->interfaces[i].ring->phys &&
									trb_ptr < keyboards[k]->interfaces[i].ring->phys + LINK_INDEX * 16u) {
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
									(mice[m]->interfaces[i].ep_num*2+1) == dci_from_event &&
									trb_ptr >= mice[m]->interfaces[i].ring->phys &&
									trb_ptr < mice[m]->interfaces[i].ring->phys + LINK_INDEX * 16u) {
									which_mouse = m;
									m_hi = &mice[m]->interfaces[i];
									break;
								}
							}
						}
						if (which_mouse >= 0) break;
					}

					if (which_kbd >= 0) {
						uint32_t requested=completed_trb->status&0x1ffffu;
						uint32_t remaining=resp.event->status&0xffffffu;
						size_t report_bytes=requested<=k_hi->mps && remaining<=requested ? requested-remaining : 0;
						if(k_hi->type==4 && k_hi->media_layout && k_hi->media_state){
							uint8_t mask=0;
							if(hid_media_decode(*k_hi->media_layout,(const uint8_t*)report,report_bytes,*k_hi->media_state,mask))
								{TtyIrqGuard guard;hid_media_update(*k_hi->media_state,mask,g_tick_count,media_key);}
						}else if(k_hi->type==1 && report_bytes>=8){
							uint8_t mask=0;
							bool legacy_receiver=keyboards[which_kbd]->vendor_id==0x1532 && keyboards[which_kbd]->interface_count==4;
							if(!legacy_receiver && k_hi->media_state && hid_boot_media_decode((const uint8_t*)report,report_bytes,mask))
								{TtyIrqGuard guard;hid_media_update(*k_hi->media_state,mask,g_tick_count,media_key);}
						int     k         = which_kbd;
						uint8_t modifiers = report[0];
                        g_kbd_modifiers=wm_hid_modifiers(modifiers); // Publish releases even with no printable keys.
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
						if (!legacy_receiver) {
							bool other_key_held=false;
							for(int b=2;b<8;++b)other_key_held|=report[b]!=0;
							wm_gui_key_update(win_taps[k],wm_hid_modifiers(modifiers),other_key_held);
							for (int b = 2; b < 8; b++) {
								uint8_t key = report[b];
							if (key == 0 || hid_boot_media_usage(key)) continue;
							if (key_down_at[k][key] == 0) {
								bool already_held=false;
								for(uint8_t previous:last_keys[k])already_held|=previous==key;
								if(already_held)continue;
								// Fresh press: record TSC time and emit immediately.
								key_down_at[k][key] = now;
								bool shift = (modifiers & 0x22u) != 0;
								uint32_t mods = wm_hid_modifiers(modifiers);
								g_kbd_modifiers = mods;

								char c = shift ? hid_to_ascii_shift[key] : hid_to_ascii[key];
								if(wm_clipboard_key(c,key,mods,false)){
									if(key<0x4F||key>0x52)key_down_at[k][key]=0;
									continue;
								}
								if (key >= 0x4F && key <= 0x52) {
									Window* focused = wm_find_window(g_focused_win_id);
									if (focused && focused->is_system && focused->type == WIN_TYPE_TERM) {
										// Shift + Up/Down = Terminal Scrollback
										if (shift && key == 0x52) term_scroll_view(-1);
										else if (shift && key == 0x51) term_scroll_view(1);
										else {
											// Regular Arrow = Shell Navigation
											tty_input('\x1b'); tty_input('[');
											char arrow_codes[] = {'C', 'D', 'B', 'A'};
											tty_input(arrow_codes[key - 0x4F]);
										}
									} else if (focused && focused->is_system && focused->type == WIN_TYPE_LOG) {
										// Boot Log focused: Up/Down scrolls log
										if (key == 0x52) wm_log_scroll(-1);
										else if (key == 0x51) wm_log_scroll(1);
									} else if (focused && !focused->is_system) {
										// User GUI window focused: send arrow event
										WinEvent ev;
										ev.type = WIN_EVENT_KEY_DOWN;
										ev.x = 0; ev.y = 0;
										char arrow_codes[] = {'C', 'D', 'B', 'A'};
										ev.key = 0x1000 + arrow_codes[key - 0x4F];
										ev.mods = mods;
										ev.delta = 0;
										wm_win_push_event(focused, ev);
									}
								} else {
									wm_dispatch_key(c, key, true, mods);
								}
							}
						}
						}
						for (int j = 0; j < 6; j++) last_keys[k][j] = report[2 + j];
						last_mods[k] = modifiers;
						}

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
						keyboards[which_kbd]->hc->doorbell32[slot_from_event] = dci_from_event;
					} else if (which_mouse >= 0) {
						int m = which_mouse;
						uint32_t requested = completed_trb->status & 0x1ffffu;
						uint32_t remaining = resp.event->status & 0xffffffu;
						size_t report_bytes = requested <= m_hi->mps && remaining <= requested ? requested - remaining : 0;
						if(m_hi->media_layout && m_hi->media_state){
							uint8_t mask=0;
							if(hid_media_decode(*m_hi->media_layout,(const uint8_t*)report,report_bytes,*m_hi->media_state,mask))
								{TtyIrqGuard guard;hid_media_update(*m_hi->media_state,mask,g_tick_count,media_key);}
						}
						if (m_hi->mouse_layout) {
							HIDMouseEvent decoded;
							if (hid_mouse_decode(*m_hi->mouse_layout, (const uint8_t*)report, report_bytes,
							                     m_hi->held_buttons, decoded)) {
								mouse_process_input(decoded.dx, decoded.dy, decoded.buttons);
								if (decoded.wheel) mouse_process_scroll(-decoded.wheel);
							}
						} else if(m_hi->type==2) {

						// Legacy report-mode fallback for existing receivers. Boot
						// reports never infer a Report ID from the button mask.
						uint8_t* data = (uint8_t*)report;
						bool shifted = false;
						if (m_hi->protocol == 0) {
							data++;
							shifted = true;
						}

						// Parse mouse report: Report Protocol mice (like wireless
						// gaming dongles) often use 16-bit LE X/Y deltas.
						// Retain their existing extended layout; new device support
						// should use descriptor-based decoding above.
						if (report_bytes >= (shifted ? 4u : 3u)) {
							uint8_t buttons = data[0];
							int16_t dx = 0, dy = 0;
							int8_t scroll = 0;

							// Auto-switcher: Use extended 16-bit path if we established this is a Razer Dongle.
							// We match the Razer's exact physical USB profile (MPS=64, 4 Interfaces) 
							// plus the keyboard traffic fingerprint so we don't accidentally
							// apply this layout to standard composite trackpad keyboards.
							bool is_razer_wireless = (mice[m]->vendor_id==0x1532 && mice[m]->is_dongle && m_hi->mps == 64 && mice[m]->interface_count == 4);
							if (m_hi->protocol != 2 || is_razer_wireless) {
								if (report_bytes >= (shifted ? 9u : 8u)) {
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
								if (report_bytes >= (shifted ? 5u : 4u)) scroll = (int8_t)data[3];
							}

							mouse_process_input(dx, dy, buttons);
							if (scroll != 0) mouse_process_scroll(-scroll);
						}
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
        }
		// Actual VRAM rendering is deferred to the 60 Hz compositor thread
		// to avoid exhausting PCIe transaction limits with 1000 Hz micro-writes.

        { KernelCpuScope cpu_scope(KCPU_HUBS);
		for (int h = 0; h < hub_count; ++h) {
			uint8_t changed_port;
            { USBControlGuard ownership;
              if (storage_owner && storage_owner != g_current_task) break;
              changed_port = hub_poll_changes(h); }
			if (changed_port) {
				usb_hub_reconcile(hubs[h], changed_port);
				goto restart;
			}
		}
        }

		// Sample all ports even if the controller misses a PSC. Require 150 ms
		// of stable CCS before port-local enumeration; also pick up a timed-out
		// port that becomes enabled later. Neither path resets mounted disks.
        { KernelCpuScope cpu_scope(KCPU_ROOT_PORTS);
        for (int ctrl_idx = 0; ctrl_idx < xhci_count; ctrl_idx++) {
            if (storage_owner && storage_owner != g_current_task) break;
            XHCIController* hc = &xhci_hc[ctrl_idx];
            if (hc->needsResetting) continue;
            uint64_t now = _usb_rdtsc();
            for (uint32_t p = 0; p < hc->max_ports; p++) {
                if (usb_root_poll(hc, p, now)) goto restart;
            }
        }
        }

		// ── Software Typematic Repeat ─────────────────────────────────────────────
		// USB keyboards do not repeat keys in hardware when SET_IDLE is configured
		// to 0. We implement standard 500ms delay + 30ms repeat rate in software.
        { KernelCpuScope cpu_scope(KCPU_KEY_REPEAT);
		uint64_t current_tsc = rdtsc();
		uint64_t ms_to_tsc = _usb_tsc_hz() / 1000;
		uint64_t repeat_delay_tsc = 500 * ms_to_tsc;
		uint64_t repeat_rate_tsc  = 30 * ms_to_tsc;

		for (int k = 0; k < keyboard_count; k++) {
			if (keyboards[k]->vendor_id==0x1532 && keyboards[k]->interface_count == 4) continue; // Legacy Razer receiver quirk.
			for (int key = 2; key < 256; key++) {
				uint64_t down_time = key_down_at[k][key];
				if (down_time != 0) {
					if ((current_tsc - down_time) > repeat_delay_tsc) {
						if (last_repeat_fire[k][key] == 0 || (current_tsc - last_repeat_fire[k][key]) > repeat_rate_tsc) {
							last_repeat_fire[k][key] = current_tsc;
							
							// Repeat key emission
							uint8_t modifiers = last_mods[k];
							bool shift = (modifiers & 0x22u) != 0;
							uint32_t mods = wm_hid_modifiers(modifiers);
							g_kbd_modifiers = mods;
							
							char c = shift ? hid_to_ascii_shift[key] : hid_to_ascii[key];
							if(wm_clipboard_key(c,key,mods,true))continue;
							if (key >= 0x4F && key <= 0x52) {
								Window* focused = wm_find_window(g_focused_win_id);
								if (focused && focused->is_system && focused->type == WIN_TYPE_TERM) {
									// Shift + Up/Down = Terminal Scrollback
									if (shift && key == 0x52) term_scroll_view(-1);
									else if (shift && key == 0x51) term_scroll_view(1);
									else {
										// Regular Arrow = Shell Navigation
										tty_input('\x1b'); tty_input('[');
										char arrow_codes[] = {'C', 'D', 'B', 'A'};
										tty_input(arrow_codes[key - 0x4F]);
									}
								} else if (focused && focused->is_system && focused->type == WIN_TYPE_LOG) {
									// Boot Log focused: Up/Down scrolls log
									if (key == 0x52) wm_log_scroll(-1);
									else if (key == 0x51) wm_log_scroll(1);
								} else if (focused && !focused->is_system) {
									// User GUI window focused: send arrow event
									WinEvent ev;
									ev.type = WIN_EVENT_KEY_DOWN;
									ev.x = 0; ev.y = 0;
									char arrow_codes[] = {'C', 'D', 'B', 'A'};
									ev.key = 0x1000 + arrow_codes[key - 0x4F];
									ev.mods = mods;
									ev.delta = 0;
									wm_win_push_event(focused, ev);
								}
							} else {
								wm_dispatch_key(c, key, true, mods,true);
							}
						}
					}
				}
			}
		}

        }
        if (disk_root_active && !g_recovery_retired && g_terminal_shell_ready) {
            // No more boot fallback after the disk shell has entered its input
            // loop. Runtime RAM mounts and the live log remain in use by VFS.
            vnode* retained[]={runtime_tmp,runtime_run,live_log};
            recovery::Released released{};
            if (recovery::release(&recovery_root,retained,3,&released)) {
                g_recovery_heap_bytes=released.bytes;
                if (kernel_address_req.response) {
                    uint64_t flags;
                    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
                    g_recovery_archive_bytes=boot_module_release(recovery_archive,module_req.response,
                        memmap_req.response,HHDM,kernel_address_req.response->physical_base,
                        (uint64_t)_kernel_end-(uint64_t)_kernel_start,free_phys_page);
                    // The separately supplied init.elf is the last-resort boot
                    // fallback. Its bytes have no consumers after disk success.
                    if (module_req.response) for (uint64_t m=0;m<module_req.response->module_count;++m) {
                        auto* mod=module_req.response->modules[m];
                        const char* base=mod ? mod->path : nullptr;
                        if (!base) continue;
                        for (const char* p=base;*p;++p) if (*p=='/') base=p+1;
                        if (strcmp(base,"init.elf")==0)
                            g_recovery_fallback_bytes+=boot_module_release(mod,module_req.response,
                                memmap_req.response,HHDM,kernel_address_req.response->physical_base,
                                (uint64_t)_kernel_end-(uint64_t)_kernel_start,free_phys_page);
                    }
                    total_usable_ram+=g_recovery_archive_bytes+g_recovery_fallback_bytes;
                    if (flags&(1u<<9)) __asm__ volatile("sti" ::: "memory");
                }
                recovery_archive=nullptr; g_recovery_retired=true;
                print((char*)"[recovery] Disk shell ready; released recovery heap bytes:");
                to_str(g_recovery_heap_bytes,str); print(str);
                print((char*)"[recovery] Released archive pages in bytes:");
                to_str(g_recovery_archive_bytes,str); print(str);
                print((char*)"[recovery] Released fallback module pages in bytes:");
                to_str(g_recovery_fallback_bytes,str); print(str);
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
    g_fatal_screen_visible = true; // hcf must preserve this exception screen

    // 2. Graphical BSOD: WyzarOS "Deep Space" theme
    uint32_t wyzar_panic_bg = 0xFF141414; // Ultra dark/black
    vram_fill(wyzar_panic_bg);

    // 3. Header
    int32_t x = 100;
    int32_t y = 100;

    vram_draw_char_8x8(x, y, ':', 0xFFFF0000);
    vram_draw_char_8x8(x + 8, y, '(', 0xFFFF0000);
    y += 40;

    vram_draw_string_8x8(x, y, "--- !!! WyzarOS CRITICAL SYSTEM FAILURE !!! ---", 0xFFFF0000); // Red alert
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

    // Retain the last boot messages on the fault screen: a hardware-only USB
    // failure must remain diagnosable after the desktop replaces the splash.
    y += 20;
    uint32_t lines = fb && fb->height > (uint32_t)y + 60 ? (fb->height - y - 60) / 12 : 0;
    if (lines > 8) lines = 8;
    if (lines && fb->width > (uint32_t)x + 8) {
        uint64_t end = g_klog_pos, start = end;
        if (start && g_klog_buffer[start - 1] == '\n') --start;
        uint32_t found = 1;
        while (start && found <= lines) {
            --start;
            if (g_klog_buffer[start] == '\n' && ++found > lines) { ++start; break; }
        }
        vram_draw_string_8x8(x, y, "RECENT KERNEL LOG:", 0xFFBBBBBB);
        y += 14;
        uint32_t columns = (fb->width - x) / 8;
        if (columns > 127) columns = 127;
        while (start < end && lines--) {
            char line[128]; uint32_t used = 0;
            while (start < end && g_klog_buffer[start] != '\n') {
                if (used < columns) line[used++] = g_klog_buffer[start];
                ++start;
            }
            if (start < end) ++start;
            line[used] = 0;
            vram_draw_string_8x8(x, y, line, 0xFFCCCCCC);
            y += 12;
        }
    }
    y += 20;
    vram_draw_string_8x8(x, y, "SYSTEM HALTED. PLEASE RESTART MANUALLY.", 0xFFFF5555);

    vram_fence();
    hcf();
}
