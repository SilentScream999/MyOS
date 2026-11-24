#include "alpha_renderer.h"
#include "window_alpha.h"
#include <emmintrin.h>

static inline __m128i blend4(__m128i src,__m128i dst) {
    auto zero=_mm_setzero_si128(),max=_mm_set1_epi16(255),bias=_mm_set1_epi16(128);
    auto lo=_mm_unpacklo_epi8(src,zero),hi=_mm_unpackhi_epi8(src,zero);
    auto a0=_mm_shufflehi_epi16(_mm_shufflelo_epi16(lo,255),255);
    auto a1=_mm_shufflehi_epi16(_mm_shufflelo_epi16(hi,255),255);
    auto d0=_mm_mullo_epi16(_mm_unpacklo_epi8(dst,zero),_mm_sub_epi16(max,a0));
    auto d1=_mm_mullo_epi16(_mm_unpackhi_epi8(dst,zero),_mm_sub_epi16(max,a1));
    // Exact round-to-nearest divide by 255 for products in [0,65025].
    d0=_mm_add_epi16(d0,bias);d1=_mm_add_epi16(d1,bias);
    d0=_mm_srli_epi16(_mm_add_epi16(d0,_mm_srli_epi16(d0,8)),8);
    d1=_mm_srli_epi16(_mm_add_epi16(d1,_mm_srli_epi16(d1,8)),8);
    auto result=_mm_or_si128(_mm_packus_epi16(_mm_add_epi16(lo,d0),_mm_add_epi16(hi,d1)),_mm_set1_epi32(0xff000000));
    // Preserve destination for alpha zero, including malformed nonzero RGB.
    auto transparent=_mm_cmpeq_epi32(_mm_srli_epi32(src,24),zero);
    return _mm_or_si128(_mm_and_si128(transparent,dst),_mm_andnot_si128(transparent,result));
}

uint64_t alpha_render(AlphaTarget target,AlphaSurface source,VisibleRect client,VisibleRect clip) {
    if(!target.pixels||!source.pixels||target.width<=0||target.height<=0||source.width<=0||source.height<=0||
       target.pitch<(int64_t)target.width*4||source.pitch<(int64_t)source.width*4||
       (target.pitch&3)||(source.pitch&3)||!visibility_valid(client))return 0;
    auto r=visibility_intersect(client,visibility_intersect(clip,{0,0,target.width,target.height}));
    if(!visibility_valid(r))return 0;
    int64_t width=(int64_t)client.x2-client.x1,height=(int64_t)client.y2-client.y1;
    if(width>16384||height>16384||source.width>16384||source.height>16384)return 0;
    for(int y=r.y1;y<r.y2;++y){
        unsigned sy=((2*((int64_t)y-client.y1)+1)*source.height)/(2*height);
        auto* row=(const uint32_t*)((const uint8_t*)source.pixels+(uint64_t)sy*source.pitch);
        auto* out=(uint32_t*)((uint8_t*)target.pixels+(uint64_t)y*target.pitch)+r.x1;
        int count=r.x2-r.x1,x=0;
        if(width==source.width){
            auto* in=row+(r.x1-client.x1);
            for(;x+4<=count;x+=4){
                auto src=_mm_loadu_si128((const __m128i*)(in+x));
                if(_mm_movemask_epi8(_mm_cmpeq_epi32(_mm_srli_epi32(src,24),_mm_setzero_si128()))==65535)continue;
                _mm_storeu_si128((__m128i*)(out+x),blend4(src,_mm_loadu_si128((const __m128i*)(out+x))));
            }
            for(;x<count;++x)out[x]=window_source_over(in[x],out[x]);
        }else{
            // Rational stepping avoids a division for every enlarged pixel.
            int64_t denominator=2*width,numerator=(2*((int64_t)r.x1-client.x1)+1)*source.width;
            unsigned sx=numerator/denominator,step=source.width/width;
            int64_t remainder=numerator%denominator,advance=2*(source.width%width);
            auto sample=[&](){
                uint32_t pixel=row[sx];sx+=step;remainder+=advance;
                if(remainder>=denominator){remainder-=denominator;++sx;}
                return pixel;
            };
            for(;x+4<=count;x+=4){
                uint32_t a=sample(),b=sample(),c=sample(),d=sample();
                auto src=_mm_set_epi32(d,c,b,a);
                if(_mm_movemask_epi8(_mm_cmpeq_epi32(_mm_srli_epi32(src,24),_mm_setzero_si128()))==65535)continue;
                _mm_storeu_si128((__m128i*)(out+x),blend4(src,_mm_loadu_si128((const __m128i*)(out+x))));
            }
            for(;x<count;++x)out[x]=window_source_over(sample(),out[x]);
        }
    }
    return (uint64_t)(r.x2-r.x1)*(r.y2-r.y1);
}
