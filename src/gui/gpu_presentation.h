#ifndef WYZAR_GPU_PRESENTATION_H
#define WYZAR_GPU_PRESENTATION_H
#include "window_overlay.h"
#include "damage.h"
#include "visibility.h"
#ifndef GPU_RETAINED_MENU
#define GPU_RETAINED_MENU 0
#endif
#ifndef GPU_EXACT_REPAIRS
#define GPU_EXACT_REPAIRS 0
#endif
// The shell still paints entire rows in these regions. Normalize damage
// before recording it for either scanout so every written pixel is tracked.
static inline void gpu_frame_damage(DamageList& frame,const DamageList& input,
                                   unsigned width,unsigned height,unsigned taskbar,bool popup,bool stats_visible=true){
    if(!width || !height || taskbar>height)return;
    for(unsigned i=0;i<input.n;++i){
        auto r=input.r[i];
        if(popup || r.y1<60 || r.y2>=height-taskbar){r.x1=0;r.x2=width-1;}
        damage_add_clipped(&frame,width,height,r.x1,r.x2,r.y1,r.y2);
    }
}
static inline bool gpu_exact_frame_rect(VisibleRegion& frame,VisibleRect r,
        unsigned width,unsigned height,unsigned taskbar,bool popup,bool stats_visible=true,int tray_left=-1){
    if(!width||!height||taskbar>height||width>INT32_MAX||height>INT32_MAX)return false;
    r=visibility_intersect(r,{0,0,(int)width,(int)height});
    if(!visibility_valid(r))return true;
    auto add=[&](VisibleRect rect){
        if(visibility_add_exact(&frame,rect))return true;
        frame.count=1;frame.rects[0]={0,0,(int)width,(int)height};return false;
    };
    if(popup)return add({0,r.y1,(int)width,r.y2});
    int shell_top=stats_visible?(height<60?(int)height:60):0,taskbar_top=height-taskbar;
    auto top=r;if(top.y2>shell_top)top.y2=shell_top;top.x1=0;top.x2=width;
    if(!add(top))return false;
    auto body=r;if(body.y1<shell_top)body.y1=shell_top;if(body.y2>taskbar_top)body.y2=taskbar_top;
    if(!add(body))return false;
    // Only callers using the opaque retained tray renderer can keep its X
    // extent. Ordinary taskbar changes retain the full-row repaint contract.
    auto bottom=r;if(bottom.y1<taskbar_top)bottom.y1=taskbar_top;
    if(tray_left<0||bottom.x1<tray_left){bottom.x1=0;bottom.x2=width;}
    return add(bottom);
}
static inline bool gpu_exact_frame_damage(VisibleRegion& frame,const DamageList& input,
        unsigned width,unsigned height,unsigned taskbar,bool popup,bool stats_visible=true,int tray_left=-1){
    for(unsigned i=0;i<input.n&&i<DAMAGE_MAX;++i){auto r=input.r[i];
        if(r.x1>r.x2||r.y1>r.y2||r.x1>=width||r.y1>=height)continue;
        if(r.x2>=width)r.x2=width-1;if(r.y2>=height)r.y2=height-1;
        if(!gpu_exact_frame_rect(frame,{(int)r.x1,(int)r.y1,(int)r.x2+1,(int)r.y2+1},width,height,taskbar,popup,stats_visible,tray_left))return false;
    }
    return true;
}

// 240 pixels per second matches the original four-pixel step at 60 Hz.
static inline uint32_t gpu_rainbow_phase(uint64_t elapsed,uint64_t hz){
    if(!hz)return 0;
    return ((elapsed/hz)%3)*240+(elapsed%hz)*240/hz;
}

// A complete cursor-free master scene plus one immutable overlay snapshot.
// Called once after all scene damage, or for a changed overlay-only frame.
// Success means a complete frame latched; failure disables the hook and leaves
// a CPU-writable live scanout for full recomposition. The master stays clean.
using gpu_present_hook_fn = bool (*)(const OverlayFrame&,const uint32_t* sprite);
extern gpu_present_hook_fn g_gpu_present_hook;
extern volatile bool g_desktop_ready;

// A CPU copy occupies the compositor core. Require a substantial latency
// improvement to justify that trade, even after including its store fence.
static inline bool gpu_scanout_use_streaming_copy(bool readback_ok,uint64_t cpu_ticks,uint64_t bcs_ticks) {
    return readback_ok && cpu_ticks && cpu_ticks <= bcs_ticks/2;
}

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
