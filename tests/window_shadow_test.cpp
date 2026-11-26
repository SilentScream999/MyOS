#include <cassert>
#include <cstdio>
#include <vector>
#include "src/gui/window_shadow.h"

static unsigned reference_alpha(VisibleRect r,int x,int y) {
    if(x>=r.x1&&x<r.x2&&y>=r.y1&&y<r.y2)return 0;
    int dy=y-4;
    int nearest_x=x<r.x1?r.x1:x>=r.x2?r.x2-1:x;
    int nearest_y=dy<r.y1?r.y1:dy>=r.y2?r.y2-1:dy;
    double distance=(x-nearest_x)*(x-nearest_x)+(dy-nearest_y)*(dy-nearest_y);
    if(distance>=144)return 0;
    double falloff=1-distance/144;
    return (unsigned)(64*falloff*falloff+1e-9);
}
static uint32_t darken(uint32_t pixel,unsigned alpha) {
    uint32_t result=0xff000000;
    for(unsigned shift=0;shift<24;shift+=8)
        result|=(unsigned)(((pixel>>shift)&255)*(1-alpha/255.0)+.5)<<shift;
    return result;
}
int main() {
    // Combined shadows must match every rounding step of repeated painting,
    // for real clipped pixels, padding, all color channels and cover geometry.
    for(unsigned repeats:{1u,2u,3u,7u,15u,32u})for(int variant=0;variant<4;++variant){
        constexpr int w=96,h=80,stride=104;
        std::vector<uint32_t> expected(stride*h+16,0xa5a5a5a5);
        for(int y=0;y<h;++y)for(int x=0;x<w;++x)expected[y*stride+x]=0xff000000u|((x*7919u+y*104729u)&0xffffffu);
        auto actual=expected;
        VisibleRect outer{12,15,75,63},clip{variant*3,variant*2,w-variant,h-variant};
        VisibleRect covers[]={outer,{35,8,90,30},{0,45,40,78}};
        for(unsigned i=0;i<repeats;++i)
            window_shadow_draw({(uint8_t*)expected.data(),w,h,stride*4},outer,clip,covers,variant);
        window_shadow_draw({(uint8_t*)actual.data(),w,h,stride*4},outer,clip,covers,variant,repeats);
        assert(actual==expected);
        // Exercise the known-row path as well; successive passes must use the
        // previous output after the first pass, rather than reuse the old row.
        std::vector<uint32_t> row(w);
        for(int x=0;x<w;++x)row[x]=0xff000000u|x*123457u;
        for(int y=0;y<h;++y)for(int x=0;x<w;++x)actual[y*stride+x]=expected[y*stride+x]=row[x];
        for(unsigned i=0;i<repeats;++i)
            window_shadow_draw({(uint8_t*)expected.data(),w,h,stride*4,i?nullptr:row.data()},outer,clip,covers,variant);
        window_shadow_draw({(uint8_t*)actual.data(),w,h,stride*4,row.data()},outer,clip,covers,variant,repeats);
        assert(actual==expected);
    }
    {
        VisibleRect a{10,10,60,60},b{20,20,70,70};
        WindowShadowLayer layers[]={{a,true,true},{a,true,false},{a,true,true},
            {b,false,true},{a,true,true},{b,true,true},{a,true,true}};
        unsigned repeats[7];window_shadow_runs(layers,7,repeats);
        unsigned expected[]={3,0,0,0,0,1,1};
        for(unsigned i=0;i<7;++i)assert(repeats[i]==expected[i]);
        std::vector<uint32_t> full(96*80,0xffaabbcc),batched=full;
        for(unsigned i=0;i<7;++i){
            if(!layers[i].visible)continue;
            auto bounds=layers[i].outer;
            if(layers[i].shadow)window_shadow_draw({(uint8_t*)full.data(),96,80,96*4},bounds,{0,0,96,80});
            window_shadow_draw({(uint8_t*)batched.data(),96,80,96*4},bounds,{0,0,96,80},nullptr,0,repeats[i]);
            // Opaque and translucent body draws remain between shadows.
            for(int y=bounds.y1;y<bounds.y2;++y)for(int x=bounds.x1;x<bounds.x2;++x){
                auto paint=[&](uint32_t p){return i&1?darken(p,100):0xff507090u+i*0x010203;};
                full[y*96+x]=paint(full[y*96+x]);batched[y*96+x]=paint(batched[y*96+x]);
            }
        }
        assert(full==batched);
    }
    static_assert(sizeof(window_shadow_cache)==169); // bounded shared cache
    constexpr int width=96,height=80,pitch=width*4+32;
    for(int test=0;test<80;++test){
        VisibleRect outer{-20+(test*7)%90,-16+(test*11)%74,0,0};
        outer.x2=outer.x1+8+(test*13)%64;outer.y2=outer.y1+10+(test*17)%62;
        VisibleRect clip{test%19,test%13,width-test%17,height-test%11};
        constexpr uint32_t guard=0xa5a5a5a5;
        auto background=[](int x,int y){return 0xff000000u|((x*7919u+y*104729u)&0xffffffu);};
        std::vector<uint32_t> pixels(pitch/4*height+32,guard);
        for(int y=0;y<height;++y)for(int x=0;x<width;++x)pixels[y*(pitch/4)+x]=background(x,y);
        VisibleRect covers[]={{10,0,35,60},{25,40,85,70}};
        unsigned cover_count=test%3;
        window_shadow_draw({(uint8_t*)pixels.data(),width,height,pitch},outer,clip,covers,cover_count);
        for(int y=0;y<height;++y)for(int x=0;x<width;++x){
            bool inside=x>=clip.x1&&x<clip.x2&&y>=clip.y1&&y<clip.y2;
            for(unsigned i=0;i<cover_count;++i)
                if(x>=covers[i].x1&&x<covers[i].x2&&y>=covers[i].y1&&y<covers[i].y2)inside=false;
            unsigned alpha=reference_alpha(outer,x,y);
            assert(window_shadow_alpha(outer,x,y)==alpha);
            assert(pixels[y*(pitch/4)+x]==(inside&&alpha?darken(background(x,y),alpha):background(x,y)));
        }
        for(int y=0;y<height;++y)for(int x=width;x<pitch/4;++x)assert(pixels[y*(pitch/4)+x]==guard);
        for(unsigned n=pitch/4*height;n<pixels.size();++n)assert(pixels[n]==guard);
        // A known background row must produce identical output, including
        // unaligned clips and row padding, without reading destination pixels.
        std::vector<uint32_t> row(width),cached(pitch/4*height+32,guard);
        for(int x=0;x<width;++x)row[x]=background(x,0);
        for(int y=0;y<height;++y)for(int x=0;x<width;++x)cached[y*(pitch/4)+x]=row[x];
        auto ordinary=cached;
        window_shadow_draw({(uint8_t*)ordinary.data(),width,height,pitch},outer,clip,covers,cover_count);
        window_shadow_draw({(uint8_t*)cached.data(),width,height,pitch,row.data()},outer,clip,covers,cover_count);
        assert(cached==ordinary);
    }
    // The same edge/corner mask is reused at any size and translation.
    VisibleRect small{10,10,40,40},large{50,70,700,900};
    assert(window_shadow_alpha(small,7,9)==window_shadow_alpha(large,47,69));
    assert(window_shadow_alpha(small,42,42)==window_shadow_alpha(large,702,902));
    for(int x=1;x<12;++x)assert(window_shadow_cache.alpha[0][x]<=window_shadow_cache.alpha[0][x-1]);
    auto bounds=window_shadow_bounds(small);
    assert(bounds.x1==-2&&bounds.y1==2&&bounds.x2==52&&bounds.y2==56);
    auto extremes=window_shadow_bounds({INT32_MIN,INT32_MIN,INT32_MAX,INT32_MAX});
    assert(extremes.x1==INT32_MIN&&extremes.y1==INT32_MIN&&extremes.x2==INT32_MAX&&extremes.y2==INT32_MAX);
    assert(!window_shadow_alpha({INT32_MIN,INT32_MIN,INT32_MIN+20,INT32_MIN+20},INT32_MAX,INT32_MAX));
    puts("Cached window shadows: clipped pixels, padding guards, offscreen geometry and resize reuse passed.");
}
