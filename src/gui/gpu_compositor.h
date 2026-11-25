#ifndef WYZAR_GPU_COMPOSITOR_H
#define WYZAR_GPU_COMPOSITOR_H

#include <stdint.h>
#include "visibility.h"
#include "damage.h"
#include "src/drivers/gpu_perf.h"
#include "gpu_opaque_coverage.h"

// Internal surface contract. No client command buffers or GPU addresses
// cross the window ABI. All rectangles have exclusive right/bottom edges.
struct GpuSceneClient {
    VisibleRect outer, client;
    const uint32_t* pixels;
    uint32_t width, height, pitch;
    bool visible, rainbow;
    bool resize_grip=true;
    bool premultiplied=false;
    uint64_t revision=0; // nonzero only for a uniquely published immutable snapshot
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

// Append disjoint clips for one source at its stack position. No submission
// occurs until the whole layer is ready; overflow must restart on the CPU.
static inline bool gpu_scene_append_clips(GpuScenePlan* batch,const GpuScenePlan& part){
    if(part.client_count!=1||part.count>GpuScenePlan::capacity||
       batch->count>GpuScenePlan::capacity-part.count)return false;
    for(unsigned i=0;i<part.count;++i)
        if(part.copies[i].source!=0||!visibility_valid(part.copies[i].destination))return false;
    if(!batch->count){
        batch->client_count=1;batch->clients[0]=part.clients[0];
        batch->scroll=part.scroll;batch->rainbow_colors=part.rainbow_colors;
    }
    for(unsigned i=0;i<part.count;++i)batch->copies[batch->count++]=part.copies[i];
    batch->accelerated=batch->count?1:0;
    return true;
}

// Canonicalize repairs before layer batching. Overlapping input must never
// blend translucent pixels twice, and a bounding box must not fill the holes.
static inline bool gpu_scene_repair_region(VisibleRegion* result,const VisibleRegion& input,
                                           VisibleRect screen,VisibleRect* bounds){
    result->count=0;
    if(!input.count||input.count>VisibleRegion::capacity)return false;
    for(unsigned i=0;i<input.count;++i){
        auto r=input.rects[i];
        if(!visibility_valid(r)||r.x1<screen.x1||r.y1<screen.y1||
           r.x2>screen.x2||r.y2>screen.y2||!visibility_add_exact(result,r))return false;
    }
    VisibleRect box=result->rects[0];
    for(unsigned i=1;i<result->count;++i){auto r=result->rects[i];
        if(r.x1<box.x1)box.x1=r.x1;if(r.y1<box.y1)box.y1=r.y1;
        if(r.x2>box.x2)box.x2=r.x2;if(r.y2>box.y2)box.y2=r.y2;
    }
    *bounds=box;return true;
}

// One GPU scene pass can clip opaque coverage inside its copy plan. Keeping
// animation strips as separate scene passes would wait on a fence per strip.
static inline bool gpu_animation_damage_bounds(const VisibleRegion& exposed,VisibleRect* bounds) {
    if(!bounds||!exposed.count||exposed.count>VisibleRegion::capacity)return false;
    VisibleRect result=exposed.rects[0];if(!visibility_valid(result))return false;
    for(unsigned i=1;i<exposed.count;++i){
        auto r=exposed.rects[i];if(!visibility_valid(r))return false;
        if(r.x1<result.x1)result.x1=r.x1;if(r.y1<result.y1)result.y1=r.y1;
        if(r.x2>result.x2)result.x2=r.x2;if(r.y2>result.y2)result.y2=r.y2;
    }
    *bounds=result;return true;
}

static inline bool gpu_scene_has_alpha(const GpuScenePlan& plan) {
    for(unsigned i=0;i<plan.client_count&&i<GpuScenePlan::max_clients;++i)
        if(plan.clients[i].visible&&plan.clients[i].premultiplied)return true;
    return false;
}

// A failure to subtract an occluder must fall back to CPU composition. Unlike
// animation damage, conservatively retaining hidden pixels would overwrite
// foreground windows when the GPU executes these copies after CPU chrome.
static inline void gpu_scene_plan(GpuScenePlan* plan, VisibleRect damage,bool include_resize_grip=false) {
    plan->count = 0;
    plan->accelerated = 0;
    if (plan->client_count > GpuScenePlan::max_clients || gpu_scene_has_alpha(*plan)) return;
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

// Refresh one procedural client on an already complete retained scanout.
// Failure must leave no copies: conservative overdraw could erase a retained
// foreground window, which this path deliberately does not repaint.
static inline bool gpu_retained_animation_plan(GpuScenePlan* plan,const GpuSceneClient& client,
        VisibleRect screen,const VisibleRect* covers,unsigned count){
    plan->count=0;plan->accelerated=0;plan->client_count=1;plan->clients[0]=client;
    if(!client.visible||!client.rainbow||client.premultiplied||!client.pixels||
       !client.width||!client.height||client.pitch!=(uint64_t)client.width*4||
       client.width>8191||client.height>32767||
       (int64_t)client.client.x2-client.client.x1!=client.width||
       (int64_t)client.client.y2-client.client.y1!=client.height)return false;
    VisibleRegion visible{};
    auto rect=visibility_intersect(client.client,screen);
    if(!visibility_valid(rect))return false;
    visible.rects[visible.count++]=rect;
    for(unsigned i=0;i<count;++i)if(!visibility_subtract(&visible,covers[i]))return false;
    if(!visible.count||visible.count>GpuScenePlan::capacity)return false;
    for(unsigned i=0;i<visible.count;++i){
        auto r=visible.rects[i];
        plan->copies[plan->count++]={0,r,(uint32_t)(r.x1-client.client.x1),(uint32_t)(r.y1-client.client.y1)};
    }
    plan->accelerated=1;return true;
}

// Remove proven opaque foreground pixels from an already valid retained plan.
// Overflow leaves the plan untouched so callers can use ordinary composition.
static inline bool gpu_retained_opaque_cutouts(GpuScenePlan* plan,const VisibleRect* covers,unsigned count){
    if(plan->client_count!=1||!plan->count||plan->count>VisibleRegion::capacity)return false;
    VisibleRegion region{};region.count=plan->count;
    for(unsigned i=0;i<plan->count;++i)region.rects[i]=plan->copies[i].destination;
    for(unsigned i=0;i<count;++i)if(!visibility_subtract(&region,covers[i]))return false;
    if(!region.count)return false;
    const auto client=plan->clients[0].client;
    for(unsigned i=0;i<region.count;++i){auto r=region.rects[i];
        plan->copies[i]={0,r,(uint32_t)(r.x1-client.x1),(uint32_t)(r.y1-client.y1)};
    }
    plan->count=region.count;return true;
}

// Animation composition replaces these pixels, including alpha above them.
// Repair only the rest of the pending scene damage, keeping exact cutouts
// rather than merging them back into bounding boxes and drawing alpha twice.
static inline bool gpu_retained_repair_region(VisibleRegion* repairs,const DamageList& damage,
                                              const GpuScenePlan& animation){
    repairs->count=0;
    if(damage.n>DAMAGE_MAX||animation.count>GpuScenePlan::capacity)return false;
    for(unsigned i=0;i<damage.n;++i){
        const auto& r=damage.r[i];
        if(r.x1>r.x2||r.y1>r.y2||r.x2>=INT32_MAX||r.y2>=INT32_MAX)return false;
        repairs->rects[repairs->count++]={(int)r.x1,(int)r.y1,(int)r.x2+1,(int)r.y2+1};
    }
    for(unsigned i=0;i<animation.count;++i)
        if(!visibility_subtract(repairs,animation.copies[i].destination))return false;
    return true;
}
// Replay an opaque cached top layer only where animation overwrote it. Keep
// it out of the occlusion plan so it cannot fragment background/alpha batches.
template<class Copy> static inline uint64_t gpu_retained_overlay_replay(
        const GpuScenePlan& animation,VisibleRect panel,Copy copy){
    uint64_t pixels=0;
    for(unsigned i=0;i<animation.count;++i){
        auto r=visibility_intersect(panel,animation.copies[i].destination);
        if(!visibility_valid(r))continue;
        copy(r);pixels+=(uint64_t)(r.x2-r.x1)*(r.y2-r.y1);
    }
    return pixels;
}
static inline bool gpu_retained_repair_region(VisibleRegion* repairs,const VisibleRegion& damage,
                                              const GpuScenePlan& animation){
    if(damage.count>VisibleRegion::capacity||animation.count>GpuScenePlan::capacity)return false;
    repairs->count=damage.count;
    for(unsigned i=0;i<damage.count;++i)repairs->rects[i]=damage.rects[i];
    for(unsigned i=0;i<animation.count;++i)
        if(!visibility_subtract(repairs,animation.copies[i].destination))return false;
    return true;
}

// Scaled opaque clients use textured RCS draws, including ordinary resized
// windows. The retained path includes the grip background, then redraws its
// small CPU decoration after GPU retirement; the generic path excludes it.
static inline void gpu_scaled_scene_plan(GpuScenePlan* plan,VisibleRect damage,bool include_resize_grip=false) {
    plan->count=0;plan->accelerated=0;
    if(plan->client_count>GpuScenePlan::max_clients||gpu_scene_has_alpha(*plan))return;
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

// Submit one translucent client at its stack position, after the CPU has
// composed the background and before any higher window chrome. The ordinary
// compositor paints its grip rows separately. Retained drawing inks the grip
// after the GPU retires and can include its background in this draw.
static inline bool gpu_alpha_scene_plan(GpuScenePlan* plan,const GpuSceneClient& c,VisibleRect damage,
                                        bool include_grip_rows=false) {
    plan->count=0;plan->accelerated=0;plan->client_count=1;plan->clients[0]=c;
    int64_t w=(int64_t)c.client.x2-c.client.x1,h=(int64_t)c.client.y2-c.client.y1;
    if(!c.visible||!c.premultiplied||c.rainbow||!c.pixels||!c.width||!c.height||
       w<=0||h<=0||w>16384||h>16384||c.width>16384||c.height>16384||
       ((uintptr_t)c.pixels&63)||c.pitch<(uint64_t)c.width*4||c.pitch>262144||(c.pitch&3))return false;
    auto r=visibility_intersect(c.client,damage);
    int bottom=c.client.y2-(c.resize_grip&&!include_grip_rows?12:0);if(r.y2>bottom)r.y2=bottom;
    if(!visibility_valid(r))return false;
    plan->copies[0]={0,r,0,0};plan->count=1;plan->accelerated=1;return true;
}

// Reblend only pixels whose animated background was replaced in this frame.
// Static pixels beneath translucent windows must retain their final composed
// value; blending onto them again would accumulate alpha every frame.
static inline bool gpu_retained_alpha_plan(GpuScenePlan* plan,const GpuSceneClient& client,
                                          const GpuScenePlan& animation){
    if(!gpu_alpha_scene_plan(plan,client,client.client,true))return false;
    plan->count=0;plan->accelerated=0;
    VisibleRect body=client.client;
    if(animation.count>GpuScenePlan::capacity)return false;
    for(unsigned i=0;i<animation.count;++i){
        auto clip=visibility_intersect(body,animation.copies[i].destination);
        if(visibility_valid(clip))plan->copies[plan->count++]={0,clip,0,0};
    }
    if(plan->count)plan->accelerated=1;
    return true;
}

// One client at its stack position: opaque/rainbow work stays on BCS even
// when a later client needs RCS alpha. 0=CPU, 1=BCS, 2=RCS.
static inline unsigned gpu_ordered_scene_plan(GpuScenePlan* plan,const GpuSceneClient& client,VisibleRect damage) {
    if(client.premultiplied)return gpu_alpha_scene_plan(plan,client,damage)?2:0;
    plan->client_count=1;plan->clients[0]=client;
    gpu_scene_plan(plan,damage);
    if(plan->count)return 1;
    gpu_scaled_scene_plan(plan,damage);
    return plan->count?2:0;
}

// Ordered work needs the pixels beneath higher shadows and alpha clients, but
// never pixels covered by higher opaque chrome/clients. On region overflow,
// retain the original draw: overdraw is safe because higher windows draw later.
static inline void gpu_ordered_occlude(GpuScenePlan* plan,const VisibleRect* opaque,unsigned count) {
    if(plan->count!=1)return;
    const auto original=plan->copies[0];
    VisibleRegion region{};region.count=1;region.rects[0]=original.destination;
    for(unsigned i=0;i<count;++i)if(!visibility_subtract(&region,opaque[i]))return;
    if(region.count>GpuScenePlan::capacity)return;
    plan->count=region.count;
    for(unsigned i=0;i<region.count;++i){
        auto r=region.rects[i];
        plan->copies[i]={original.source,r,
            original.source_x+(uint32_t)(r.x1-original.destination.x1),
            original.source_y+(uint32_t)(r.y1-original.destination.y1)};
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
       rcs.count>GpuScenePlan::capacity||gpu_scene_has_alpha(bcs)||gpu_scene_has_alpha(rcs)||
       (!bcs.count&&!rcs.count))return false;
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
    if(!visibility_valid(damage)||plan->client_count>GpuScenePlan::max_clients||gpu_scene_has_alpha(*plan))return false;
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
    if(!visibility_valid(damage) || plan->client_count>GpuScenePlan::max_clients||gpu_scene_has_alpha(*plan))return false;
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
