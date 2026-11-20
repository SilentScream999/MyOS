#ifndef WYZAR_GPU_PRESENTATION_H
#define WYZAR_GPU_PRESENTATION_H
#include "window_overlay.h"

// A complete cursor-free master scene plus one immutable overlay snapshot.
// Called once after all scene damage, or for a changed overlay-only frame.
// Success means a complete frame latched; failure disables the hook and leaves
// a CPU-writable live scanout for full recomposition. The master stays clean.
using gpu_present_hook_fn = bool (*)(const OverlayFrame&,const uint32_t* sprite);
extern gpu_present_hook_fn g_gpu_present_hook;
extern volatile bool g_desktop_ready;

static inline bool gpu_overlay_equal(const OverlayFrame& a,const OverlayFrame& b) {
    if(a.cursor.x!=b.cursor.x || a.cursor.y!=b.cursor.y ||
       a.cursor.edges!=b.cursor.edges || a.cursor.buttons!=b.cursor.buttons || a.count!=b.count)return false;
    for(unsigned i=0;i<a.count;++i){
        const auto x=a.outlines[i],y=b.outlines[i];
        if(x.x!=y.x || x.y!=y.y || x.w!=y.w || x.h!=y.h)return false;
    }
    return true;
}
#endif
