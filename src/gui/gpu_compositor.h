#ifndef WYZAR_GPU_COMPOSITOR_H
#define WYZAR_GPU_COMPOSITOR_H

#include <stdint.h>
#include "visibility.h"

// Internal opaque-surface contract. No client command buffers or GPU addresses
// cross the window ABI. All rectangles have exclusive right/bottom edges.
struct GpuSceneClient {
    VisibleRect outer, client;
    const uint32_t* pixels;
    uint32_t width, height, pitch;
    bool visible, rainbow;
    bool resize_grip=true;
};
struct GpuSceneCopy {
    unsigned source;
    VisibleRect destination;
    uint32_t source_x, source_y;
};
struct GpuScenePlan {
    static constexpr unsigned max_clients = 32, capacity = 256;
    GpuSceneClient clients[max_clients]; // bottom to top
    GpuSceneCopy copies[capacity];
    unsigned client_count, count;
    uint32_t accelerated;
    uint32_t scroll;
    const uint32_t* rainbow_colors;
};
using gpu_compose_hook_fn = bool (*)(const GpuScenePlan&);
extern gpu_compose_hook_fn g_gpu_compose_hook;
extern gpu_compose_hook_fn g_gpu_render_hook;
extern bool g_gpu_quarantine_sources;
extern uint64_t g_gpu_surface_epoch;

// A failure to subtract an occluder must fall back to CPU composition. Unlike
// animation damage, conservatively retaining hidden pixels would overwrite
// foreground windows when the GPU executes these copies after CPU chrome.
static inline void gpu_scene_plan(GpuScenePlan* plan, VisibleRect damage,bool include_resize_grip=false) {
    plan->count = 0;
    plan->accelerated = 0;
    if (plan->client_count > GpuScenePlan::max_clients) return;
    for (unsigned i = 0; i < plan->client_count; ++i) {
        const auto& client = plan->clients[i];
        if (!client.visible || !client.pixels || !client.width || !client.height ||
            (int64_t)client.client.x2-client.client.x1 != client.width ||
            (int64_t)client.client.y2-client.client.y1 != client.height ||
            client.width > 32767 || client.height > 32767 ||
            client.pitch < (uint64_t)client.width*4 || client.pitch > 32767 ||
            (client.pitch & 3) || (!client.rainbow && ((uintptr_t)client.pixels & 63))) continue;
        VisibleRegion visible{};
        auto rect = visibility_intersect(client.client, damage);
        if (!visibility_valid(rect)) continue;
        // The resize grip is still a CPU decoration; keep its last twelve rows
        // on the CPU so GPU copies cannot erase it.
        int bottom = client.client.y2-(client.resize_grip&&!include_resize_grip?12:0);
        if (rect.y2 > bottom) rect.y2 = bottom;
        if (!visibility_valid(rect)) continue;
        visible.rects[visible.count++] = rect;
        bool valid = true;
        for (unsigned j = i+1; j < plan->client_count && valid; ++j)
            if (plan->clients[j].visible)
                valid = visibility_subtract(&visible, plan->clients[j].outer);
        if (!valid || visible.count > GpuScenePlan::capacity-plan->count) continue;
        plan->accelerated |= uint32_t(1) << i;
        for (unsigned j = 0; j < visible.count; ++j) {
            auto r = visible.rects[j];
            plan->copies[plan->count++] = {i,r,(uint32_t)(r.x1-client.client.x1),
                                            (uint32_t)(r.y1-client.client.y1)};
        }
    }
}

// Scaled opaque clients use textured RCS draws, including ordinary resized
// windows. The retained path includes the grip background, then redraws its
// small CPU decoration after GPU retirement; the generic path excludes it.
static inline void gpu_scaled_scene_plan(GpuScenePlan* plan,VisibleRect damage,bool include_resize_grip=false) {
    plan->count=0;plan->accelerated=0;
    if(plan->client_count>GpuScenePlan::max_clients)return;
    for(unsigned i=0;i<plan->client_count;++i){
        const auto& c=plan->clients[i];
        int64_t width=(int64_t)c.client.x2-c.client.x1;
        int64_t height=(int64_t)c.client.y2-c.client.y1;
        if(!c.visible||c.rainbow||!c.pixels||!c.width||!c.height||width<=0||height<=0||
           width>16384||height>16384||c.width>16384||c.height>16384||
           (width==(int)c.width&&height==(int)c.height)||((uintptr_t)c.pixels&63)||
           c.pitch<c.width*4||c.pitch>262144||(c.pitch&3))continue;
        auto r=visibility_intersect(c.client,damage);
        int bottom=c.client.y2-(c.resize_grip&&!include_resize_grip?12:0);
        if(r.y2>bottom)r.y2=bottom;
        if(!visibility_valid(r))continue;
        VisibleRegion visible{};visible.rects[visible.count++]=r;
        bool valid=true;
        for(unsigned j=i+1;j<plan->client_count&&valid;++j)
            if(plan->clients[j].visible)valid=visibility_subtract(&visible,plan->clients[j].outer);
        if(!valid||visible.count>GpuScenePlan::capacity-plan->count)continue;
        plan->accelerated|=1u<<i;
        for(unsigned j=0;j<visible.count;++j)plan->copies[plan->count++]={i,visible.rects[j],0,0};
    }
}

// Reuse the existing scene if every damaged window pixel has a GPU copy.
// Remaining rectangles may contain desktop background only. Any uncovered
// chrome or unsupported client requires the full CPU scene path. Subtraction
// handles occlusion and gaps without assuming that window areas are disjoint.
static inline bool gpu_retained_damage(const GpuScenePlan& bcs,const GpuScenePlan& rcs,
                                      VisibleRect damage,VisibleRegion* background) {
    if(!background||!visibility_valid(damage)||bcs.client_count>GpuScenePlan::max_clients||
       rcs.client_count!=bcs.client_count||bcs.count>GpuScenePlan::capacity||
       rcs.count>GpuScenePlan::capacity||(!bcs.count&&!rcs.count))return false;
    background->count=1;background->rects[0]=damage;
    const GpuScenePlan* plans[]={&bcs,&rcs};
    for(const auto* plan:plans)for(unsigned i=0;i<plan->count;++i){
        auto r=plan->copies[i].destination;
        if(plan->copies[i].source>=plan->client_count||!visibility_valid(r)||
           r.x1<damage.x1||r.y1<damage.y1||r.x2>damage.x2||r.y2>damage.y2||
           !visibility_subtract(background,r))return false;
    }
    for(unsigned n=0;n<background->count;++n)
        for(unsigned i=0;i<bcs.client_count;++i)
            if(bcs.clients[i].visible&&visibility_valid(
                visibility_intersect(background->rects[n],bcs.clients[i].outer)))return false;
    return true;
}

static inline bool gpu_scene_pixel_exposed(const GpuScenePlan& plan,unsigned source,int x,int y) {
    for(unsigned j=source+1;j<plan.client_count;++j){
        const auto& c=plan.clients[j];
        if(c.visible&&x>=c.outer.x1&&x<c.outer.x2&&y>=c.outer.y1&&y<c.outer.y2)return false;
    }
    return true;
}

static inline bool gpu_scaled_damage_plan(GpuScenePlan* plan,VisibleRect damage) {
    if(!visibility_valid(damage)||plan->client_count>GpuScenePlan::max_clients)return false;
    for(unsigned n=plan->client_count;n>0;--n){
        unsigned i=n-1;const auto& c=plan->clients[i];
        if(!c.visible||!visibility_valid(visibility_intersect(c.outer,damage)))continue;
        if(c.resize_grip||damage.x1<c.client.x1||damage.y1<c.client.y1||
           damage.x2>c.client.x2||damage.y2>c.client.y2)return false;
        gpu_scaled_scene_plan(plan,damage);
        if(plan->count!=1||plan->copies[0].source!=i)return false;
        const auto r=plan->copies[0].destination;
        return r.x1==damage.x1&&r.y1==damage.y1&&r.x2==damage.x2&&r.y2==damage.y2;
    }
    return false;
}

// A maximized opaque rainbow owns every damaged pixel inside its
// client. Reuse the retained master outside that damage instead of repainting
// covered windows, their chrome, wallpaper and shell UI on every animation tick.
// The caller excludes open shell popups before selecting this path.
static inline bool gpu_rain_damage_plan(GpuScenePlan* plan,VisibleRect damage) {
    if(!visibility_valid(damage) || plan->client_count>GpuScenePlan::max_clients)return false;
    for(unsigned n=plan->client_count;n>0;--n){
        unsigned i=n-1;const auto& c=plan->clients[i];
        if(!c.visible)continue;
        if(!visibility_valid(visibility_intersect(c.outer,damage)))continue;
        if(!c.rainbow || c.resize_grip || !c.pixels ||
           c.width>32767 || c.height>32767 ||
           c.width!=(int64_t)c.client.x2-c.client.x1 || c.height!=(int64_t)c.client.y2-c.client.y1 ||
           c.pitch<(uint64_t)c.width*4 || c.pitch>32767 || (c.pitch&3) ||
           damage.x1<c.client.x1 || damage.y1<c.client.y1 ||
           damage.x2>c.client.x2 || damage.y2>c.client.y2)return false;
        plan->count=1;plan->accelerated=uint32_t(1)<<i;
        plan->copies[0]={i,damage,(uint32_t)(damage.x1-c.client.x1),(uint32_t)(damage.y1-c.client.y1)};
        return true;
    }
    return false;
}

#endif
