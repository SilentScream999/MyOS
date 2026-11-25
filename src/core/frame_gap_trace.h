#ifndef WYZAR_FRAME_GAP_TRACE_H
#define WYZAR_FRAME_GAP_TRACE_H
#if GPU_FRAME_DIAGNOSTICS
#include "../drivers/gpu_frame_gap.h"
#include "../drivers/gpu_launch_trace.h"
#include "tty.h"

static GpuGapHistory g_gpu_gap_history{};
struct GpuGapScope {
    uint64_t serial;
    GpuGapScope(GpuGapActivity activity,const char* detail=nullptr) {
        TtyIrqGuard guard;
        serial=g_gpu_gap_history.begin(gpu_launch_now(),activity,
            g_current_task?g_current_task->pid:-1,detail,(guard.flags&(1u<<9))!=0);
    }
    ~GpuGapScope(){
        TtyIrqGuard guard;
        g_gpu_gap_history.finish(serial,gpu_launch_now());
    }
};
#endif
#endif
