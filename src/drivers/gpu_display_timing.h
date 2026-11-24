#ifndef WYZAR_GPU_DISPLAY_TIMING_H
#define WYZAR_GPU_DISPLAY_TIMING_H
#include <stdint.h>
#ifndef GPU_FRAME_DIAGNOSTICS
#define GPU_FRAME_DIAGNOSTICS 0
#endif
// Observations only. A stable read is not an exact vblank/flip deadline.
// PIPEDSL can need platform/output-dependent scanline offsets in i915:
// https://codebrowser.dev/linux/linux/drivers/gpu/drm/i915/display/intel_vblank.c.html
struct GpuDisplayPhase {
    uint64_t first=0,last=0;
    uint32_t frame=0,raw_line=0,live=0;
    bool valid=false;
    uint32_t line() const{return raw_line&0xfffffu;}
};
template<class Read,class Clock> static inline GpuDisplayPhase gpu_read_display_phase(
        Read read,Clock clock,uint32_t frame_reg,uint32_t line_reg,uint32_t live_reg){
    GpuDisplayPhase p{};p.first=clock();
    p.frame=read(frame_reg);p.raw_line=read(line_reg);p.live=read(live_reg);
    uint32_t final_frame=read(frame_reg);p.last=clock();
    p.valid=p.frame!=UINT32_MAX&&p.frame==final_frame&&p.raw_line!=UINT32_MAX&&
        p.live!=UINT32_MAX&&p.last>=p.first;
    return p;
}
#endif
