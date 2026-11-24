#ifndef WYZAR_GPU_OCCLUSION_METRICS_H
#define WYZAR_GPU_OCCLUSION_METRICS_H
#include "visibility.h"
struct GpuOcclusionLayer {VisibleRect outer,client;bool alpha=false;};
struct GpuOcclusionMetric {
    uint64_t cover_pixels=0,newly_hidden_pixels=0,lower_alpha_client_pixels=0;
};
static inline uint64_t gpu_occlusion_area(VisibleRect r){
    return visibility_valid(r)?(uint64_t)((int64_t)r.x2-r.x1)*((int64_t)r.y2-r.y1):0;
}
// Diagnostic only: ordinary opaque windows/title bars, before proven opaque
// source cutouts. Layer order is bottom to top, above the animated client.
static inline bool gpu_occlusion_metrics(VisibleRect animation,VisibleRect screen,
        const GpuOcclusionLayer* layers,unsigned count,GpuOcclusionMetric* out){
    VisibleRect bounds=visibility_intersect(animation,screen);
    VisibleRegion visible{};if(visibility_valid(bounds))visible.rects[visible.count++]=bounds;
    for(unsigned i=0;i<count;++i){
        out[i]={};auto cover=layers[i].outer;
        if(layers[i].alpha)cover.y2=layers[i].client.y1;
        cover=visibility_intersect(cover,bounds);
        out[i].cover_pixels=gpu_occlusion_area(cover);
        uint64_t before=0,after=0;
        for(unsigned j=0;j<visible.count;++j)before+=gpu_occlusion_area(visible.rects[j]);
        if(!visibility_subtract(&visible,cover))return false;
        for(unsigned j=0;j<visible.count;++j)after+=gpu_occlusion_area(visible.rects[j]);
        out[i].newly_hidden_pixels=before-after;
        for(unsigned j=0;j<i;++j)if(layers[j].alpha)
            out[i].lower_alpha_client_pixels+=gpu_occlusion_area(visibility_intersect(cover,layers[j].client));
    }
    return true;
}
#endif
