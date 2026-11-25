#ifndef WYZAR_GPU_PERF_H
#define WYZAR_GPU_PERF_H
#include <stdint.h>
#include "gpu_launch_trace.h"
#include "gpu_execution_timing.h"
#include "gpu_workload.h"
#include "gpu_display_timing.h"
struct GpuPerfSleep {
    uint64_t calls=0,ticks=0,at_least_2ms=0,at_least_10ms=0;
};
static inline void gpu_perf_record_sleep(GpuPerfSleep& stats,uint64_t ticks,uint64_t hz) {
    ++stats.calls;stats.ticks+=ticks;
    if(hz){
        if(ticks>=hz/500)++stats.at_least_2ms;
        if(ticks>=hz/100)++stats.at_least_10ms;
    }
}
struct GpuPerfCounters {
    uint64_t bcs_batches=0,bcs_wait=0,rcs_draws=0,rcs_wait=0;
    uint64_t upload_bytes=0,uploads=0,upload_reuses=0,upload_ticks=0,handoff_ticks=0;
    uint64_t scene_ticks=0,present_ticks=0;
    uint64_t present_calls=0,scanout_copy_ticks=0,overlay_ticks=0,flip_ticks=0;
    uint64_t scanout_cpu_copies=0,direct_flips=0;
    uint64_t cursor_restores=0;
    uint64_t repair_rects=0,repair_pixels=0,scanout_seed_bytes=0;
    uint64_t menu_ticks=0,menu_renders=0,menu_reuses=0;
    uint64_t retained_animation_frames=0,retained_animation_pixels=0;
    uint64_t retained_alpha_frames=0;
    uint64_t shadow_row_frames=0;
    uint64_t opaque_retained_pixels=0;
    uint64_t opaque_scans=0,opaque_scan_ticks=0;
    uint64_t alpha_repair_frames=0,alpha_repair_rects=0,alpha_repair_cancels=0;
    uint64_t menu_cache_repairs=0,opaque_repair_rects=0;
    uint64_t retained_menu_frames=0,retained_menu_pixels=0;
    uint64_t retained_menu_close_deferrals=0;
    uint64_t exact_repair_frames=0,exact_repair_pixels=0;
    uint64_t scene_repair_batches=0,scene_repair_rects=0;
    uint64_t tray_repair_rects=0,tray_repair_pixels=0,tray_repair_ticks=0;
    uint64_t transition_rebases=0,transition_rebase_failed=0;
    uint64_t cpu_alpha_draws=0,cpu_alpha_pixels=0,cpu_alpha_ticks=0;
    uint64_t rcs_alpha_pixels=0,rcs_alpha_skipped=0;
    uint64_t alpha_tail_windows=0,alpha_tail_ticks=0,alpha_tail_prediction_us=0;
    GpuPerfSleep bcs_sleep{},rcs_sleep{},flip_sleep{};
};
inline GpuPerfCounters g_gpu_perf{};
enum class ScanoutCheck {
    Readback,
    Present,
    Gpu,
    Render,
    PresentHook,
    ComposeHook,
    RenderHook,
    Policy,
    Rainbow,
    Maximized,
    Menu,
    Clipboard,
    Taskbar,
    Back,
    Front,
    Alias,
    Live,
    Surface,
    Damage,
    Band,
    TopRow,
    BottomRow,
    Visible,
    Minimized,
    X,
    Y,
    Width,
    Height,
    Stack,
    Above,
    Alpha,
    Overlap,Geometry,Count
};
inline constexpr const char* scanout_check_names[]={
    "readbackVerified",
    "presentReady",
    "gpuReady",
    "renderReady",
    "presentHookMatches",
    "composeHookMatches",
    "renderHookMatches",
    "rcsAlphaPolicy",
    "rainbowPresent",
    "rainbowMaximized",
    "menuClosed",
    "clipboardClosed",
    "taskbarPopupClosed",
    "backBufferAvailable",
    "frontBufferAvailable",
    "backIsHidden",
    "livePlaneBase",
    "armedPlaneBase",
    "damageCount",
    "bandBottomExclusive",
    "damageFirstRow",
    "damageLastRow",
    "rainbowVisible",
    "rainbowRestored",
    "rainbowX",
    "rainbowY",
    "rainbowWidth",
    "rainbowHeight",
    "rainbowInStack",
    "visibleWindowsAbove",
    "translucentWindowsAbove",
    "windowsOnScreenAboveRainbow",
    "animationBandChanged"
};
static constexpr unsigned scanout_check_count=(unsigned)ScanoutCheck::Count;
static_assert(sizeof(scanout_check_names)/sizeof(*scanout_check_names)==scanout_check_count);
struct ScanoutRejection {
    uint64_t count;
    int64_t actual,expected; // Last observation of this check in the sample.
};
inline ScanoutRejection g_scanout_rejections[scanout_check_count]{};
static inline bool reject_scanout(ScanoutCheck check,int64_t actual=0,int64_t expected=1){
    auto& sample=g_scanout_rejections[(unsigned)check];
    ++sample.count;sample.actual=actual;sample.expected=expected;
    return false;
}
// Wall-clock timestamps from the same Rainbow update. For multiple alpha
// submissions retain the first submit and the last confirmed retirement.
struct RainbowWindowTrace {
    int id=0;
    char title[40]{};
    bool alpha=false;
    uint64_t start=0,end=0,shadow=0,chrome=0,gpu=0,client=0;
    uint64_t bcs=0,rcs=0,passes=0;
};
struct RainbowFrameTrace {
    uint64_t update=0,alpha_submit=0,alpha_done=0,copy_submit=0,copy_done=0;
    uint64_t arm=0,latch=0,interval=0,alpha_draws=0;
    uint64_t simd_start=0,simd_done=0;
    RainbowWindowTrace windows[32]{};
};
inline RainbowFrameTrace g_rain_trace{},g_rain_slowest{};

struct GpuFlipCadence {
    uint64_t last_tick=0,last_frame=0;
    uint32_t last_refresh=0;
    uint64_t intervals=0,total_ticks=0,max_ticks=0,last_interval=0;
    uint64_t intervening_refreshes=0,over_two_refreshes=0;

    void record(uint64_t tick,uint64_t frame,uint32_t refresh) {
        // Exclude idle periods with no presentation on intervening loops.
        if(last_tick && frame==last_frame+1){
            last_interval=tick-last_tick;
            ++intervals;
            total_ticks+=last_interval;
            if(last_interval>max_ticks)max_ticks=last_interval;
            uint32_t elapsed=refresh-last_refresh;
            if(elapsed && elapsed<0x80000000u){
                intervening_refreshes+=elapsed-1;
                if(elapsed>2)++over_two_refreshes;
            }
        }else last_interval=0;
        last_tick=tick;
        last_frame=frame;
        last_refresh=refresh;
    }
    void reset_sample() {
        intervals=total_ticks=max_ticks=intervening_refreshes=over_two_refreshes=0;
    }
};
inline GpuFlipCadence g_gpu_flip_cadence{};
// Whole-driver CPU/sleep elapsed time, used only for coarse diagnostic samples.
struct GpuPerfTimer {
    uint64_t& total;uint64_t started;
    static uint64_t now(){uint32_t lo,hi;__asm__ volatile("rdtsc":"=a"(lo),"=d"(hi)::"memory");return ((uint64_t)hi<<32)|lo;}
    GpuPerfTimer(uint64_t& counter):total(counter),started(now()){}
    ~GpuPerfTimer(){total+=now()-started;}
};
// CPU observations of one complete frame. Ready means the compositor observed
// the completion fence, not a timestamp of hardware execution finishing.
struct GpuFrameTimeline {
#if GPU_FRAME_DIAGNOSTICS
    struct Cost {uint64_t wall=0,cpu=0,invalid=0;};
    enum Phase {Prepare,Direct,Merge,Pack,RepairPlan,RepairPaint,Revalidate,MenuBuild,MenuReplay,Fallback,PhaseCount};
    Cost planning[PhaseCount]{};
    uint64_t sleep_to_ready=0,ready_to_resume=0,ready_late_ticks=0,resume_late_ticks=0;
    int sleep_ready_pid=-1;
    struct RcsCompletion {
        uint64_t submitted=0,observed=0,wait_ticks=0;
        uint64_t max_sleep=0,sleep_to_ready=0,ready_to_resume=0;
        int sleep_ready_pid=-1;
        uint32_t sequence=0;
        GpuExecutionTiming hardware{};
    } slowest_rcs;
    GpuDisplayPhase display_previous{},display_start{},display_before{},display_after{},display_latch{};
#endif
#if GPU_WORKLOAD
    GpuWorkload workload{};
#endif
    bool measure=false;
    GpuExecutionTiming bcs_gpu{},rcs_gpu{};
    uint64_t begin=0,previous_latch=0,compose=0,first_submit=0,last_ready=0;
    uint64_t arm=0,latch=0,finish=0,cpu=0,bcs=0,rcs=0,sleep=0;
    uint64_t bcs_batches=0,rcs_draws=0,refreshes=0;
    uint64_t flip_polls=0,max_flip_sleep=0,first_refresh=0,first_live=0;
    uint64_t interval() const {return previous_latch&&latch>=previous_latch?latch-previous_latch:0;}
};
inline GpuFrameTimeline g_gpu_timeline{},g_gpu_timeline_slowest{};
inline GpuFrameTimeline g_gpu_execution_slowest{};
inline uint64_t g_gpu_timeline_last_latch=0;
#if GPU_FRAME_DIAGNOSTICS
inline GpuDisplayPhase g_gpu_previous_phase{};
struct GpuCpuPhaseTimer {
    GpuFrameTimeline::Cost* cost;uint64_t (*cpu_clock)();uint64_t (*wall_clock)();
    uint64_t wall=0,cpu=0;
    GpuCpuPhaseTimer(GpuFrameTimeline::Cost* target,uint64_t (*cpu_now)(),uint64_t (*wall_now)()=GpuPerfTimer::now)
        :cost(target),cpu_clock(cpu_now),wall_clock(wall_now){if(cost){wall=wall_clock();cpu=cpu_clock();}}
    ~GpuCpuPhaseTimer(){if(!cost)return;uint64_t cpu_end=cpu_clock(),wall_end=wall_clock();
        if(cpu_end<cpu||wall_end<wall){++cost->invalid;return;}
        cost->wall+=wall_end-wall;cost->cpu+=cpu_end-cpu;
    }
};
static inline GpuFrameTimeline::Cost* gpu_phase_cost(GpuFrameTimeline::Phase phase){
    return g_gpu_timeline.begin?&g_gpu_timeline.planning[phase]:nullptr;
}
#endif
static inline void gpu_timeline_submit(uint64_t now){
    if(g_gpu_timeline.begin&&!g_gpu_timeline.first_submit)g_gpu_timeline.first_submit=now;
}
static inline void gpu_timeline_ready(uint64_t now){
    if(g_gpu_timeline.begin)g_gpu_timeline.last_ready=now;
}
static inline void gpu_timeline_finish(uint64_t now,uint64_t cpu,const GpuPerfCounters& stats){
    auto& trace=g_gpu_timeline;
    if(!trace.begin)return;
    trace.finish=now;trace.cpu=cpu-trace.cpu;
    trace.bcs=stats.bcs_wait-trace.bcs;trace.rcs=stats.rcs_wait-trace.rcs;
    trace.sleep=stats.bcs_sleep.ticks+stats.rcs_sleep.ticks+stats.flip_sleep.ticks-trace.sleep;
    trace.bcs_batches=stats.bcs_batches-trace.bcs_batches;
    trace.rcs_draws=stats.rcs_draws-trace.rcs_draws;
    if(trace.latch){
        if(trace.measure&&(trace.bcs_gpu.samples||trace.rcs_gpu.samples||trace.bcs_gpu.invalid||trace.rcs_gpu.invalid)&&
           trace.interval()>=g_gpu_execution_slowest.interval())g_gpu_execution_slowest=trace;
        if(trace.interval()>=g_gpu_timeline_slowest.interval())g_gpu_timeline_slowest=trace;
        g_gpu_timeline_last_latch=trace.latch;
    }else g_gpu_timeline_last_latch=0; // Exclude idle/software-only intervals.
    trace.begin=0;
}
struct MenuOpenTrace {
    uint64_t opened=0,requested=0,resource_start=0,resource_done=0,resource_commit=0;
    uint64_t resource_cpu=0,cache_start=0,cache_done=0,allocation_ticks=0,build_ticks=0;
    uint64_t first_present=0,max_interval=0,bcs_wall=0,bcs_cpu=0,bcs_sleep=0;
    uint64_t bcs_start=0,bcs_done=0;
    bool refresh=false,printed=false,gpu=false;
};
inline MenuOpenTrace g_menu_open_traces[8]{};
inline unsigned g_menu_open_trace_count=0;
inline uint64_t g_menu_open_last_present=0;
static inline MenuOpenTrace* gpu_menu_open_trace(){
    if(!GPU_RCS_ALPHA || !g_menu_open_trace_count)return nullptr;
    auto* trace=&g_menu_open_traces[g_menu_open_trace_count-1];
    return trace->printed?nullptr:trace;
}
static inline void gpu_menu_begin(){
    if(!GPU_RCS_ALPHA || g_menu_open_trace_count==8)return;
    g_menu_open_traces[g_menu_open_trace_count++].opened=GpuPerfTimer::now();
}
static inline void gpu_menu_present(uint64_t tick,bool gpu){
    uint64_t previous=g_menu_open_last_present;g_menu_open_last_present=tick;
    auto* trace=gpu_menu_open_trace();
    if(!trace)return;
    if(!trace->first_present){trace->first_present=tick;trace->gpu=gpu;}
    uint64_t start=previous>trace->opened?previous:trace->opened;
    if(tick-start>trace->max_interval)trace->max_interval=tick-start;
}
// One stack-order window visit. Scope exit also captures clipped/failed visits.
struct RainbowWindowTimer {
    RainbowWindowTrace* sample;
    uint64_t started=0,bcs=0,rcs=0;
    uint64_t* phase=nullptr;
    RainbowWindowTimer(unsigned slot,int id,const char* title,bool alpha):sample(
        g_rain_trace.update && slot<32?&g_rain_trace.windows[slot]:nullptr){
        if(!sample)return;
        started=GpuPerfTimer::now();
        if(!sample->passes){
            sample->id=id;sample->alpha=alpha;sample->start=started;
            for(unsigned i=0;i<sizeof(sample->title)-1 && title[i];++i)sample->title[i]=title[i];
        }
        ++sample->passes;
        phase=&sample->shadow;
        bcs=g_gpu_perf.bcs_wait;rcs=g_gpu_perf.rcs_wait;
    }
    void next(uint64_t RainbowWindowTrace::* field){
        if(!sample)return;
        uint64_t now=GpuPerfTimer::now();
        *phase+=now-started;started=now;phase=&(sample->*field);
    }
    ~RainbowWindowTimer(){
        if(!sample)return;
        sample->end=GpuPerfTimer::now();*phase+=sample->end-started;
        sample->bcs+=g_gpu_perf.bcs_wait-bcs;sample->rcs+=g_gpu_perf.rcs_wait-rcs;
    }
};
// Measure the elapsed return time of the existing one-ms wait, including
// scheduler delay. This wrapper does not change its blocking policy.
template<class Sleep>
static inline void gpu_perf_sleep_one(GpuPerfSleep& stats,uint64_t hz,Sleep sleep) {
    uint64_t started=GpuPerfTimer::now();
    sleep(1);
    gpu_perf_record_sleep(stats,GpuPerfTimer::now()-started,hz);
}
#endif
