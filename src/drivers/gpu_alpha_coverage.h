#ifndef WYZAR_GPU_ALPHA_COVERAGE_H
#define WYZAR_GPU_ALPHA_COVERAGE_H
#include "gpu_render_commands.h"
#ifndef GPU_ALPHA_MERGE
#define GPU_ALPHA_MERGE 0
#endif

// Disjoint rectangles for one source and one affine sampling transform only.
// Joining a full shared edge preserves their exact union, including all holes.
static inline unsigned gpu_alpha_merge_rects(GpuRenderRect* rects,unsigned count){
    bool changed;
    do{
        changed=false;
        for(unsigned i=0;i<count&&!changed;++i)for(unsigned j=i+1;j<count;++j){
            auto a=rects[i],b=rects[j];
            bool vertical=a.x1==b.x1&&a.x2==b.x2&&(a.y2==b.y1||b.y2==a.y1);
            bool horizontal=a.y1==b.y1&&a.y2==b.y2&&(a.x2==b.x1||b.x2==a.x1);
            if(!vertical&&!horizontal)continue;
            rects[i]={a.x1<b.x1?a.x1:b.x1,a.y1<b.y1?a.y1:b.y1,
                      a.x2>b.x2?a.x2:b.x2,a.y2>b.y2?a.y2:b.y2};
            for(unsigned k=j+1;k<count;++k)rects[k-1]=rects[k];
            --count;changed=true;break;
        }
    }while(changed);
    return count;
}

// Summarize the staged image in horizontal bands. Only zero BGRA is guaranteed
// to be a no-op for the existing ONE / INV_SRC_ALPHA blend state.
struct GpuAlphaCoverage {
    static constexpr unsigned band_count = GPU_RENDER_MAX_RECTS/2;
    struct Band { unsigned left, right, top, bottom, gap_start, gap_end; };
    Band bands[band_count]{};

    void scan(const uint8_t* pixels, unsigned width, unsigned height, unsigned pitch) {
        for (unsigned i = 0; i < band_count; ++i) {
            Band& band = bands[i];
            band = {width, 0, height*i/band_count, height*(i+1)/band_count, 0, 0};
            if (band.top == band.bottom) continue;
            const unsigned first = band.top ? band.top-1 : 0;
            const unsigned end = band.bottom < height ? band.bottom+1 : height;
            for (unsigned x = 0; x < width; ++x) {
                bool occupied = false;
                for (unsigned y = first; y < end; ++y) {
                    const auto* row = reinterpret_cast<const uint32_t*>(pixels + (uint64_t)y*pitch);
                    if (row[x]) { occupied = true; break; }
                }
                if (!occupied) continue;
                if (band.left == width) band.left = x;
                else if (x-band.right > band.gap_end-band.gap_start) {
                    band.gap_start = band.right;
                    band.gap_end = x;
                }
                band.right = x+1;
            }
        }
    }

    unsigned clips(const GpuTextureDraw& draw, GpuRenderRect* out,bool merge=false) const {
        const int width = draw.client.x2 - draw.client.x1;
        const int height = draw.client.y2 - draw.client.y1;
        unsigned count = 0;
        for (const Band& band : bands) {
            if (band.left >= band.right) continue;
            // Widen the source coverage to allow for sampler rounding. Y uses
            // exact pixel-center band boundaries with guard rows in scan().
            auto boundary = [&](unsigned y) {
                const uint64_t scaled = 2ull*y*height+draw.source.height;
                return draw.client.y1 + (int)((scaled-1)/(2ull*draw.source.height));
            };
            auto append = [&](unsigned left, unsigned right) {
                if (left) --left;
                GpuRenderRect r{
                    draw.client.x1 + (int)((uint64_t)left*width/draw.source.width),
                    boundary(band.top),
                    draw.client.x1 + (int)(((uint64_t)(right+1)*width+draw.source.width-1)/draw.source.width),
                    boundary(band.bottom)};
                if (r.x1 < draw.clip.x1) r.x1 = draw.clip.x1;
                if (r.y1 < draw.clip.y1) r.y1 = draw.clip.y1;
                if (r.x2 > draw.clip.x2) r.x2 = draw.clip.x2;
                if (r.y2 > draw.clip.y2) r.y2 = draw.clip.y2;
                if (r.x1 < r.x2 && r.y1 < r.y2) out[count++] = r;
            };
            // Both rectangles need guard texels. Keep one rectangle when
            // expansion and destination rounding consume the whole gap.
            const auto gap_left = ((uint64_t)(band.gap_start+1)*width+draw.source.width-1)/draw.source.width;
            const auto gap_right = band.gap_end ? (uint64_t)(band.gap_end-1)*width/draw.source.width : 0;
            if (gap_right > gap_left) {
                append(band.left, band.gap_start);
                append(band.gap_end, band.right);
            } else {
                append(band.left, band.right);
            }
        }
        return merge?gpu_alpha_merge_rects(out,count):count;
    }
    bool append_clips(const GpuTextureDraw& draw,GpuRenderRect* out,unsigned& count,bool merge=false) const {
        if(count>GPU_RENDER_MAX_RECTS)return false;
        GpuRenderRect next[GPU_RENDER_MAX_RECTS];
        unsigned added=clips(draw,next,merge);
        if(merge){
            // A full batch may still accept more fragments when exact edge
            // joins reduce the combined list. Keep failure transactional.
            GpuRenderRect combined[GPU_RENDER_MAX_RECTS*2];
            for(unsigned i=0;i<count;++i)combined[i]=out[i];
            for(unsigned i=0;i<added;++i)combined[count+i]=next[i];
            unsigned total=gpu_alpha_merge_rects(combined,count+added);
            if(total>GPU_RENDER_MAX_RECTS)return false;
            for(unsigned i=0;i<total;++i)out[i]=combined[i];
            count=total;return true;
        }
        if(added>GPU_RENDER_MAX_RECTS-count)return false;
        for(unsigned i=0;i<added;++i)out[count++]=next[i];
        return true;
    }
};
#endif
