#ifndef WYZAR_ALPHA_RENDERER_H
#define WYZAR_ALPHA_RENDERER_H
#include "visibility.h"
#include <stdint.h>
struct AlphaSurface { const uint32_t* pixels;int width,height,pitch; };
struct AlphaTarget { uint32_t* pixels;int width,height,pitch; };
// Same nearest-texel-center sampling and premultiplied source-over as RCS.
// Caller owns both surfaces until the draw returns. No allocation or GPU wait.
uint64_t alpha_render(AlphaTarget target,AlphaSurface source,VisibleRect client,VisibleRect clip);
static inline bool alpha_cpu_eligible(VisibleRect client,VisibleRect damage,uint64_t limit) {
    auto r=visibility_intersect(client,damage);
    return limit&&visibility_valid(r)&&(uint64_t)((int64_t)r.x2-r.x1)*((int64_t)r.y2-r.y1)<=limit;
}
#endif
