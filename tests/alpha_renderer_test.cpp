#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <chrono>
#include "src/gui/alpha_renderer.h"
#include "src/gui/window_alpha.h"

static uint32_t state=1234567;
static uint32_t random_pixel(){state=state*1664525u+1013904223u;return state;}
static void compare(int sw,int sh,VisibleRect client,VisibleRect clip) {
    constexpr int w=83,h=71,stride=89;
    std::vector<uint32_t> source((sw+3)*sh),actual(stride*h+16),expected;
    for(auto& pixel:source)pixel=random_pixel();
    for(unsigned i=0;i<source.size();i+=7)source[i]&=0xffffff;
    for(unsigned i=1;i<source.size();i+=7)source[i]|=0xff000000;
    for(auto& pixel:actual)pixel=random_pixel();expected=actual;
    auto r=visibility_intersect(client,visibility_intersect(clip,{0,0,w,h}));
    uint64_t count=0;
    for(int y=r.y1;y<r.y2;++y)for(int x=r.x1;x<r.x2;++x){
        unsigned sx=(2LL*(x-client.x1)+1)*sw/(2LL*(client.x2-client.x1));
        unsigned sy=(2LL*(y-client.y1)+1)*sh/(2LL*(client.y2-client.y1));
        expected[y*stride+x]=window_source_over(source[sy*(sw+3)+sx],expected[y*stride+x]);++count;
    }
    assert(alpha_render({actual.data(),w,h,stride*4},{source.data(),sw,sh,(sw+3)*4},client,clip)==count);
    assert(actual==expected); // also checks row padding and allocation guards
}
int main(){
    for(unsigned pass=0;pass<1000;++pass){
        int sw=1+random_pixel()%70,sh=1+random_pixel()%65;
        int x=(int)(random_pixel()%40)-20,y=(int)(random_pixel()%40)-20;
        int w=1+random_pixel()%120,h=1+random_pixel()%100;
        compare(sw,sh,{x,y,x+w,y+h},{0,0,83,71});
        compare(sw,sh,{x,y,x+sw,y+sh},{13,17,62,55});
    }
    for(unsigned a=0;a<256;++a)for(unsigned d=0;d<256;++d){
        uint32_t source[4]={a<<24|0x102030,a<<24|0xffffff,a<<24,a<<24|0x002040};
        uint32_t dest[4]={0xff000000|d*0x10101,0xff445566,0xffabcdef,0xff102030},expected[4];
        for(unsigned i=0;i<4;++i)expected[i]=window_source_over(source[i],dest[i]);
        alpha_render({dest,4,1,16},{source,4,1,16},{0,0,4,1},{0,0,4,1});
        for(unsigned i=0;i<4;++i)assert(dest[i]==expected[i]);
    }
    assert(alpha_cpu_eligible({0,0,1024,1024},{0,0,1024,1024},1024*1024));
    assert(!alpha_cpu_eligible({0,0,1024,1025},{0,0,1024,1025},1024*1024));
    assert(!alpha_cpu_eligible({0,0,10,10},{0,0,10,10},0));
    assert(!alpha_cpu_eligible({0,0,10,10},{20,20,30,30},1024*1024));
    assert(alpha_cpu_eligible({0,0,2256,1504},{0,0,2256,1504},UINT64_MAX));
    // Informational host throughput; not a physical Intel comparison.
    std::vector<uint32_t> source(320*240,0x80402010),dest(900*650,0xff103050);
    auto started=std::chrono::steady_clock::now();
    for(unsigned i=0;i<200;++i)
        alpha_render({dest.data(),900,650,900*4},{source.data(),320,240,320*4},{0,0,900,650},{0,0,900,650});
    double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count()/200;
    printf("SIMD alpha: randomized scaling/clipping, padding, exhaustive alpha/channel and path limits passed. Host 900x650 draw %.3f ms (not a GPU benchmark).\n",ms);
}
