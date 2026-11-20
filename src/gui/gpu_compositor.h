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
extern bool g_gpu_quarantine_sources;
extern uint64_t g_gpu_surface_epoch;

// A failure to subtract an occluder must fall back to CPU composition. Unlike
// animation damage, conservatively retaining hidden pixels would overwrite
// foreground windows when the GPU executes these copies after CPU chrome.
static inline void gpu_scene_plan(GpuScenePlan* plan, VisibleRect damage) {
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
        int bottom = client.client.y2-(client.resize_grip?12:0);
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
