#include <cassert>
#include <climits>
#include <cstdio>
#include "../src/gui/visibility.h"

static bool contains(VisibleRect r,int x,int y) {
    return x>=r.x1 && x<r.x2 && y>=r.y1 && y<r.y2;
}
int main() {
    // Compare exact pixel coverage against a reference mask, including stacked
    // overlapping occluders, edge touching, holes and off-screen rectangles.
    unsigned seed=12345;
    auto random=[&](){seed=seed*1664525u+1013904223u;return seed;};
    for (int trial=0;trial<500;++trial) {
        VisibleRegion region{};
        VisibleRect client=visibility_clip((int)(random()%40)-10,(int)(random()%40)-10,
                                            random()%35,random()%35,0,32,32);
        if (visibility_valid(client)) region.rects[region.count++]=client;
        bool mask[32][32]{};
        for(int y=0;y<32;++y) for(int x=0;x<32;++x) mask[y][x]=contains(client,x,y);
        for(int step=0;step<16;++step) {
            VisibleRect cover=visibility_clip((int)(random()%40)-10,(int)(random()%40)-10,
                                                random()%30,random()%30,3,32,32);
            assert(visibility_subtract(&region,cover));
            for(int y=0;y<32;++y) for(int x=0;x<32;++x) {
                mask[y][x] &= !contains(cover,x,y);
                unsigned hits=0;
                for(unsigned i=0;i<region.count;++i) hits+=contains(region.rects[i],x,y);
                assert(hits==(unsigned)mask[y][x]);
            }
        }
    }
    assert(!visibility_valid(visibility_clip(INT_MAX,INT_MAX,100,100,0,32,32)));
    assert(!visibility_valid(visibility_clip(INT_MIN,INT_MIN,100,100,20,32,32)));
    assert(visibility_valid(visibility_clip(-20,-20,INT_MAX,INT_MAX,20,32,32)));
    // A central vertical cut would exceed capacity. No partial mutation allowed.
    VisibleRegion full{}; full.count=VisibleRegion::capacity;
    for(unsigned i=0;i<full.count;++i) full.rects[i]={0,(int)i*2,10,(int)i*2+1};
    assert(!visibility_subtract(&full,{4,0,6,200}));
    assert(full.count==VisibleRegion::capacity);
    for(unsigned i=0;i<full.count;++i) {
        assert(full.rects[i].x1==0 && full.rects[i].x2==10);
        assert(full.rects[i].y1==(int)i*2 && full.rects[i].y2==(int)i*2+1);
    }
    assert(visibility_subtract(&full,{0,0,10,200}) && !full.count);
    puts("Visibility coverage, clipping and capacity tests passed.");
}
