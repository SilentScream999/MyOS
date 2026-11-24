#include "window_shadow.h"
#include "window_alpha.h"
#include <emmintrin.h>

static void shadow_span(uint32_t* row,int left,int right,int y,VisibleRect outer,const uint32_t* background){
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
                        const VisibleRect* opaque,unsigned opaque_count) {
    if(!target.pixels||target.width<=0||target.height<=0||
       target.pitch<(int64_t)target.width*4||(target.pitch&3)||!visibility_valid(outer))return;
    auto region=visibility_intersect(window_shadow_bounds(outer),
        visibility_intersect(clip,{0,0,target.width,target.height}));
    if(!visibility_valid(region))return;
    VisibleRegion exposed{};exposed.count=1;exposed.rects[0]=region;
    if(!visibility_subtract(&exposed,outer))return;
    for(unsigned i=0;i<opaque_count;++i)
        if(!visibility_subtract(&exposed,opaque[i]))break;
    // Subtraction keeps the input on overflow. Any remaining overdraw is
    // covered by the higher opaque layer later in stack order.
    for(unsigned i=0;i<exposed.count;++i){
        const auto& r=exposed.rects[i];
        for(int y=r.y1;y<r.y2;++y){
            auto* row=(uint32_t*)(target.pixels+(uint64_t)y*target.pitch);
            shadow_span(row,r.x1,r.x2,y,outer,target.background_row);
        }
    }
}
