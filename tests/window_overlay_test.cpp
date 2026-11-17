#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../src/gui/window_decorations.h"
#include "../src/gui/window_overlay.h"
#include "../src/gui/overlay_renderer.h"

static uint32_t cursor_color(const OverlayCursor& c,int x,int y) {
    if(!c.edges)return 0;
    auto pixel=resize_cursor_pixel(c.edges,x,y);
    return pixel==2?0xFFF8F3FF:pixel==1?0xFF17121E:0;
}
static uint32_t reference(const OverlayFrame& f,int x,int y,uint32_t background) {
    for(unsigned i=0;i<f.count;++i){auto r=f.outlines[i];
        if(r.w>0&&r.h>0&&x>=r.x&&x<r.x+r.w&&y>=r.y&&y<r.y+r.h&&
           (x<r.x+2||x>=r.x+r.w-2||y<r.y+2||y>=r.y+r.h-2))background=0xFFFFFFFF;
    }
    auto color=cursor_color(f.cursor,x-f.cursor.x,y-f.cursor.y);
    return color?color:background;
}
int main(){
    constexpr int width=700,height=96,stride=720;
    std::vector<uint32_t> background(stride*height),screen(stride*height),expected(stride*height);
    for(unsigned i=0;i<background.size();++i)background[i]=0xFF000000+i;
    unsigned writes=0,seed=12345;
    auto random=[&](){seed=seed*1664525u+1013904223u;return seed;};
    for(unsigned edges:{0u,1u,2u,4u,8u,5u,6u,9u,10u})for(int trial=0;trial<24;++trial){
        OverlayFrame f{};
        int cx=(int)(random()%(width+10))-5,cy=(int)(random()%(height+10))-5;
        f.cursor={cx,cy,edges,1};f.count=trial%3;
        f.outlines[0]={cx-2,cy-2,270,60};
        // Exercise thin/tiny/clipped outlines and overlapping corners.
        f.outlines[1]={(int)(random()%width)-10,(int)(random()%height)-10,(int)(random()%550),(int)(random()%100)};
        screen=background;expected=background;
        for(int y=0;y<height;++y)for(int x=0;x<width;++x)
            expected[y*stride+x]=reference(f,x,y,background[y*stride+x]);
        auto present=[&](int left,int right,int y){
            if(y<0||y>=height)return;
            if(left<0)left=0;if(right>width)right=width;
            overlay_present_row(screen.data()+y*stride,background.data()+y*stride,left,right,y,f,cursor_color,
                [&](uint32_t* d,const uint32_t* s,int bytes){
                    assert(bytes>0&&bytes<=256*4&&bytes%4==0);
                    auto offset=d-screen.data();
                    // Check EVERY write, including erasing old pixels: a final
                    // screenshot alone cannot catch a blank corner in between.
                    for(int i=0;i<bytes/4;++i)assert(s[i]==expected[offset+i]);
                    std::memcpy(d,s,bytes);++writes;
                });
        };
        for(int y=0;y<height;++y)present(0,width,y);
        assert(screen==expected);
        // Repaint the old cursor footprint and border strips crossing the new
        // corner, as on pointer motion and stationary compositor refreshes.
        for(int move=-8;move<=8;++move)for(int y=cy-12;y<cy+20;++y){
            present(cx+move-12,cx+move+20,y);
            present(cx+move,cx+move+2,y);
        }
        assert(screen==expected);
        for(unsigned i=0;i<background.size();++i)assert(background[i]==0xFF000000+i);
    }
    // Consecutive frames, rather than repeated erasure within a static frame:
    // scene blits can introduce a snapshot before the overlay's final pass.
    // Window release/destruction must erase it without consulting live windows.
    screen=background;auto rendered=background;
    OverlayFrame previous{{-999,-999,0,0},{},0};
    for(int step=0;step<240;++step){
        OverlayFrame next{};
        unsigned edges[]={1u,4u,5u,6u,9u,10u};
        next.cursor={(int)(random()%(width+20))-10,(int)(random()%(height+20))-10,edges[step%6],1};
        next.count=step%7==0?0:step%3+1;
        for(unsigned i=0;i<next.count;++i)
            next.outlines[i]={(int)(random()%(width+40))-20,(int)(random()%(height+40))-20,
                              (int)(random()%width)+1,(int)(random()%height)+1};
        if(step==239){next.count=0;next.cursor={-999,-999,0,0};}
        expected=background;
        for(int y=0;y<height;++y)for(int x=0;x<width;++x)
            expected[y*stride+x]=reference(next,x,y,background[y*stride+x]);
        auto present=[&](int left,int right,int y){
            if(left<0)left=0;if(right>width)right=width;
            overlay_present_row(screen.data()+y*stride,background.data()+y*stride,left,right,y,next,cursor_color,
                [&](uint32_t* d,const uint32_t* s,int bytes){
                    auto offset=d-screen.data();
                    for(int i=0;i<bytes/4;++i)assert(s[i]==expected[offset+i]);
                    std::memcpy(d,s,bytes);++writes;
                });
        };
        // Partial scene repaints at new positions, including multiple dirty
        // rectangles in the same frame and pointer motion between snapshots.
        if(step%2==0)for(int y=4;y<20;++y)present(10,450,y);
        overlay_transition(previous,next,height,present);
        assert(screen==expected);
        uint32_t sprite[CURSOR_SPRITE_SIZE*CURSOR_SPRITE_SIZE];
        for(int y=0;y<CURSOR_SPRITE_SIZE;++y)for(int x=0;x<CURSOR_SPRITE_SIZE;++x)
            sprite[y*CURSOR_SPRITE_SIZE+x]=cursor_color(next.cursor,x-CURSOR_SPRITE_ORIGIN,y-CURSOR_SPRITE_ORIGIN);
        OverlayTarget target{(uint8_t*)rendered.data(),(const uint8_t*)background.data(),width,height,stride*4};
        overlay_render_transition(target,previous,next,sprite,false);
        __asm__ volatile("sfence" ::: "memory");
        assert(rendered==expected);
        previous=next;
    }
    assert(screen==background);
    OverlayFrame still{{20,20,0,0},{{10,10,200,80}},1};
    unsigned spans=0;
    overlay_transition(still,still,height,[&](int,int,int){++spans;});assert(spans==0);
    auto moved=still;moved.cursor.x+=3;
    overlay_transition(still,moved,height,[&](int,int,int){++spans;});assert(spans<=64);
    spans=0;overlay_transition(still,still,height,[&](int,int,int){++spans;},true);
    assert(spans>64); // Scene damage must reapply even unchanged overlays.
    std::printf("Overlay: %u writes and 240 moving/released/closed snapshots leave no fragments.\n",writes);
}
