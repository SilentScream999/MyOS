#ifndef WYZAR_GPU_WORKLOAD_H
#define WYZAR_GPU_WORKLOAD_H
#include <stdint.h>
#ifndef GPU_WORKLOAD
#define GPU_WORKLOAD 0
#endif
enum class GpuCopyKind {Other,Rainbow,Client,Scanout,Count};
struct GpuCopyWork {uint64_t ops=0,bytes=0;};
// Logical command payload, not measured DRAM traffic: copies read and write
// this many bytes. Includes repeated/self copies; excludes pitch padding.
struct GpuWorkload {
    GpuCopyWork copies[(unsigned)GpuCopyKind::Count]{};
    uint64_t fill_ops=0,fill_bytes=0,fences=0;
    uint64_t rcs_draws=0,rcs_rects=0,blend_pixels=0,opaque_pixels=0;
};
// Input rectangles have already passed the texture batch's bounds and overlap
// checks. Count the submitted geometry, including any alpha-zero texels within it.
template<class Rect> static inline void gpu_workload_render(GpuWorkload& work,
        const Rect* rects,unsigned count,bool blend){
    ++work.rcs_draws;work.rcs_rects+=count;
    for(unsigned i=0;i<count;++i){
        const auto& r=rects[i];
        uint64_t pixels=(uint64_t)(r.x2-r.x1)*(r.y2-r.y1);
        (blend?work.blend_pixels:work.opaque_pixels)+=pixels;
    }
}
#endif
