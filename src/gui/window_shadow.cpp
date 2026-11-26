#include "window_shadow.h"
#include "window_alpha.h"
#include <emmintrin.h>

// Repeated black shadows are a per-channel mapping. Cache exact 8-bit rounding
// at every step, rather than approximate the stack with one combined alpha.
struct ShadowRepeatMap { unsigned count;uint8_t values[65][256]; };
// Eight counts cover all distinct repeated run lengths in a 32-window stack,
// so different coincident groups do not rebuild tables on every frame.
static ShadowRepeatMap shadow_repeat_maps[8];
static unsigned shadow_repeat_next=0;
static const ShadowRepeatMap* prepare_shadow_repeat(unsigned count){
    for(const auto& map:shadow_repeat_maps)if(map.count==count)return &map;
    auto& map=shadow_repeat_maps[shadow_repeat_next++%8];
    for(unsigned alpha=0;alpha<=64;++alpha)for(unsigned value=0;value<256;++value){
        unsigned result=value;
        for(unsigned i=0;i<count;++i){
            unsigned product=result*(255-alpha)+128;
            result=(product+(product>>8))>>8;
        }
        map.values[alpha][value]=result;
    }
    map.count=count;return &map;
}
static void shadow_span(uint32_t* row,int left,int right,int y,VisibleRect outer,const uint32_t* background,const ShadowRepeatMap* repeated){
    if(repeated){
        for(int x=left;x<right;++x){
            unsigned alpha=window_shadow_alpha(outer,x,y);if(!alpha)continue;
            uint32_t pixel=(background?background:row)[x];const auto* map=repeated->values[alpha];
            row[x]=0xff000000u|(uint32_t(map[(pixel>>16)&255])<<16)|
                (uint32_t(map[(pixel>>8)&255])<<8)|map[pixel&255];
        }
        return;
    }
    auto zero=_mm_setzero_si128(),bias=_mm_set1_epi16(128);
    int x=left;
    for(;x+4<=right;x+=4){
        unsigned a=window_shadow_alpha(outer,x,y),b=window_shadow_alpha(outer,x+1,y);
        unsigned c=window_shadow_alpha(outer,x+2,y),d=window_shadow_alpha(outer,x+3,y);
        if(!(a|b|c|d))continue;
        auto original=_mm_loadu_si128((const __m128i*)((background?background:row)+x));
        auto low_factor=_mm_set_epi16(255-b,255-b,255-b,255-b,255-a,255-a,255-a,255-a);
        auto high_factor=_mm_set_epi16(255-d,255-d,255-d,255-d,255-c,255-c,255-c,255-c);
        auto lo=_mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(original,zero),low_factor),bias);
        auto hi=_mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(original,zero),high_factor),bias);
        lo=_mm_srli_epi16(_mm_add_epi16(lo,_mm_srli_epi16(lo,8)),8);
        hi=_mm_srli_epi16(_mm_add_epi16(hi,_mm_srli_epi16(hi,8)),8);
        auto result=_mm_or_si128(_mm_packus_epi16(lo,hi),_mm_set1_epi32(0xff000000));
        auto untouched=_mm_cmpeq_epi32(_mm_set_epi32(d,c,b,a),zero);
        result=_mm_or_si128(_mm_and_si128(untouched,original),_mm_andnot_si128(untouched,result));
        _mm_storeu_si128((__m128i*)(row+x),result);
    }
    for(;x<right;++x){
        unsigned alpha=window_shadow_alpha(outer,x,y);
        if(alpha)row[x]=window_source_over(alpha<<24,(background?background:row)[x]);
    }
}

void window_shadow_draw(WindowShadowTarget target,VisibleRect outer,VisibleRect clip,
                        const VisibleRect* opaque,unsigned opaque_count,unsigned repetitions) {
    if(!target.pixels||target.width<=0||target.height<=0||
       target.pitch<(int64_t)target.width*4||(target.pitch&3)||!visibility_valid(outer)||!repetitions)return;
    auto region=visibility_intersect(window_shadow_bounds(outer),
        visibility_intersect(clip,{0,0,target.width,target.height}));
    if(!visibility_valid(region))return;
    VisibleRegion exposed{};exposed.count=1;exposed.rects[0]=region;
    if(!visibility_subtract(&exposed,outer))return;
    for(unsigned i=0;i<opaque_count;++i){
        // The window body was already removed. Covers wholly inside it cannot
        // hide a shadow, and repeated identical windows commonly supply these.
        const auto cover=opaque[i];
        if(cover.x1>=outer.x1&&cover.y1>=outer.y1&&cover.x2<=outer.x2&&cover.y2<=outer.y2)continue;
        if(!visibility_subtract(&exposed,opaque[i]))break;
    }
    const auto* repeated=repetitions>1?prepare_shadow_repeat(repetitions):nullptr;
    // Subtraction keeps the input on overflow. Any remaining overdraw is
    // covered by the higher opaque layer later in stack order.
    for(unsigned i=0;i<exposed.count;++i){
        const auto& r=exposed.rects[i];
        for(int y=r.y1;y<r.y2;++y){
            auto* row=(uint32_t*)(target.pixels+(uint64_t)y*target.pitch);
            shadow_span(row,r.x1,r.x2,y,outer,target.background_row,repeated);
        }
    }
}
