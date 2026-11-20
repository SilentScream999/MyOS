#ifndef WYZAR_GPU_SCENE_COMMANDS_H
#define WYZAR_GPU_SCENE_COMMANDS_H

#include "gpu_cmds.h"
#include "gpu_compositor.h"

// The immutable 720-pixel palette is uploaded once. Build one shifted row with
// BLTs, then double the initialized rows with non-overlapping self-copies. No
// per-frame full-size rainbow upload or CPU surface generation is required.
static inline bool gpu_emit_rainbow(GpuRing* ring, uint64_t palette,
                                    uint64_t destination, uint32_t pitch,
                                    const GpuSceneCopy& copy, uint32_t scroll) {
    const auto r = copy.destination;
    uint32_t width = r.x2-r.x1, height = r.y2-r.y1;
    uint32_t position = ((uint64_t)scroll+copy.source_x)%720;
    for (uint32_t x = 0; x < width;) {
        uint32_t n = 720-position;
        if (n > width-x) n = width-x;
        if (!gpu_emit_copy(ring,palette,2880,position,0,
                           destination,pitch,r.x1+x,r.y1,n,1)) return false;
        x += n; position = 0;
    }
    for (uint32_t rows = 1; rows < height;) {
        uint32_t n = rows < height-rows ? rows : height-rows;
        // Serialize prior destination writes before using those pixels as a
        // source. These barriers are in the batch, with no CPU fence waits.
        uint32_t sequence;
        if (!gpu_emit_fence(ring,&sequence,false) ||
            !gpu_emit_copy(ring,destination,pitch,r.x1,r.y1,
                           destination,pitch,r.x1,r.y1+rows,width,n)) return false;
        rows += n;
    }
    return true;
}

#endif
