#ifndef WYZAR_GPU_OPAQUE_COVERAGE_H
#define WYZAR_GPU_OPAQUE_COVERAGE_H
#include "visibility.h"
#ifndef GPU_OPAQUE_RETAIN
#define GPU_OPAQUE_RETAIN 0
#endif
// Conservative opaque source rectangles, cached only for immutable published
// revisions. Unproven pixels remain on the ordinary rendering path.
struct GpuOpaqueCoverage {
    static constexpr unsigned capacity=8;
    const uint32_t* pixels=nullptr;
    uint64_t revision=0;
    unsigned width=0,height=0,pitch=0,count=0;
    VisibleRect source[capacity]{};
    bool update(const uint32_t*,unsigned,unsigned,unsigned,uint64_t);
    unsigned mapped(VisibleRect client,int grip,VisibleRect* out) const;
};
#endif
