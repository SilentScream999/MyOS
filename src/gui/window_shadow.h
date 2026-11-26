#ifndef WYZAR_WINDOW_SHADOW_H
#define WYZAR_WINDOW_SHADOW_H
#include "visibility.h"

// A shared nine-slice alpha mask: corners use both distances, horizontal and
// vertical edges use distance zero on the other axis. No per-window bitmap,
// allocations, blur, or resize-time cache rebuilds are needed.
struct WindowShadowCache {
    static constexpr int radius=12, offset_y=4;
    uint8_t alpha[radius+1][radius+1]{};
    constexpr WindowShadowCache() {
        for(int y=0;y<=radius;++y)for(int x=0;x<=radius;++x){
            int remaining=radius*radius-x*x-y*y;
            alpha[y][x]=remaining>0?64*remaining*remaining/(radius*radius*radius*radius):0;
        }
    }
};
inline constexpr WindowShadowCache window_shadow_cache{};

static inline VisibleRect window_shadow_bounds(VisibleRect outer) {
    auto bounded=[](int64_t value){return (int32_t)(value<INT32_MIN?INT32_MIN:value>INT32_MAX?INT32_MAX:value);};
    return {bounded((int64_t)outer.x1-WindowShadowCache::radius),
            bounded((int64_t)outer.y1+WindowShadowCache::offset_y-WindowShadowCache::radius),
            bounded((int64_t)outer.x2+WindowShadowCache::radius),
            bounded((int64_t)outer.y2+WindowShadowCache::offset_y+WindowShadowCache::radius)};
}
static inline uint8_t window_shadow_alpha(VisibleRect outer,int x,int y) {
    if(x>=outer.x1&&x<outer.x2&&y>=outer.y1&&y<outer.y2)return 0;
    int64_t yy=(int64_t)y-WindowShadowCache::offset_y;
    int64_t dx=x<outer.x1?(int64_t)outer.x1-x:x>=outer.x2?(int64_t)x-outer.x2+1:0;
    int64_t dy=yy<outer.y1?outer.y1-yy:yy>=outer.y2?yy-outer.y2+1:0;
    if(dx>WindowShadowCache::radius||dy>WindowShadowCache::radius)return 0;
    return window_shadow_cache.alpha[dy][dx];
}
struct WindowShadowTarget {
    uint8_t* pixels;int width,height,pitch;
    // Optional exact background for every affected row. Caller guarantees
    // that it matches the target before this shadow, including prior layers.
    const uint32_t* background_row=nullptr;
};
struct WindowShadowLayer { VisibleRect outer;bool visible,shadow; };
// Equal-bounds clients never touch the shadow outside their shared rectangle.
// Consecutive shadows can therefore be replayed as one exact repeated blend,
// even when one of those clients is translucent. Different bounds split runs.
static inline void window_shadow_runs(const WindowShadowLayer* layers,unsigned count,unsigned* repeats){
    for(unsigned i=0;i<count;++i)repeats[i]=layers[i].visible&&layers[i].shadow?1:0;
    for(unsigned i=0;i<count;++i){
        if(!repeats[i])continue;
        const auto a=layers[i].outer;
        for(unsigned j=i+1;j<count;++j){
            if(!layers[j].visible)continue;
            const auto b=layers[j].outer;
            if(a.x1!=b.x1||a.y1!=b.y1||a.x2!=b.x2||a.y2!=b.y2)break;
            repeats[i]+=repeats[j];repeats[j]=0;
        }
    }
}
// Target is the opaque desktop master. Clip and outer bounds are exclusive.
void window_shadow_draw(WindowShadowTarget target,VisibleRect outer,VisibleRect clip,
                        const VisibleRect* opaque=nullptr,unsigned opaque_count=0,unsigned repetitions=1);
#endif
