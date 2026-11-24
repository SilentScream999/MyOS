#ifndef WYZAR_GPU_SCENE_COMMANDS_H
#define WYZAR_GPU_SCENE_COMMANDS_H

#include "gpu_cmds.h"
#include "gpu_compositor.h"
static constexpr unsigned GPU_RAIN_TILE_ROWS=256;
static constexpr uint64_t GPU_RAIN_TILE_BYTES=720*4*GPU_RAIN_TILE_ROWS;

// The immutable 720-pixel palette is uploaded once. Build one shifted row with
// BLTs, then double the initialized rows with non-overlapping self-copies. No
// per-frame full-size rainbow upload or CPU surface generation is required.
static inline bool gpu_emit_rainbow(GpuRing* ring, uint64_t palette,
                                    uint64_t destination, uint32_t pitch,
                                    const GpuSceneCopy& copy, uint32_t scroll,unsigned tile_rows=1,
                                    bool wide_blits=false) {
    const auto r = copy.destination;
    uint32_t width = r.x2-r.x1, height = r.y2-r.y1;
    uint32_t position = ((uint64_t)scroll+copy.source_x)%720;
    if(tile_rows==GPU_RAIN_TILE_ROWS && width<=4096 && height<=2160){
        // Every copy reads the immutable tile. No destination pixels become
        // later sources, eliminating the serial row-doubling barriers.
        // Every 720 consecutive texels repeat, even across a tile row.
        // Reinterpret the same allocation at a wider pitch for shorter runs
        // of full-width BLTs. Keep the legacy layout when it costs fewer.
        if(wide_blits){
            unsigned aligned_phase=position & ~15u; // BLT bases are 64-byte aligned
            unsigned sx=position-aligned_phase;
            unsigned source_width=((sx+width+719)/720)*720;
            unsigned rows=(720*tile_rows-aligned_phase)/source_width;
            unsigned old_commands=((position+width+719)/720)*((height+tile_rows-1)/tile_rows);
            if(rows&&(height+rows-1)/rows<old_commands){
                for(unsigned y=0;y<height;y+=rows){
                    unsigned n=height-y<rows?height-y:rows;
                    if(!gpu_emit_copy(ring,palette+aligned_phase*4,source_width*4,sx,0,
                        destination,pitch,r.x1,r.y1+y,width,n))return false;
                }
                return true;
            }
        }
        for(uint32_t y=0;y<height;){
            uint32_t rows=height-y;if(rows>tile_rows)rows=tile_rows;
            uint32_t pos=position;
            for(uint32_t x=0;x<width;){
                uint32_t columns=720-pos;if(columns>width-x)columns=width-x;
                if(!gpu_emit_copy(ring,palette,2880,pos,0,destination,pitch,
                                  r.x1+x,r.y1+y,columns,rows))return false;
                x+=columns;pos=0;
            }
            y+=rows;
        }
        return true;
    }
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
