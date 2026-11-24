#ifndef WYZAR_WINDOW_OVERLAY_H
#define WYZAR_WINDOW_OVERLAY_H
#include <stdint.h>

constexpr int CURSOR_SPRITE_SIZE=32,CURSOR_SPRITE_ORIGIN=12;
struct OverlayCursor { int x,y; unsigned edges; uint8_t buttons; };
struct OverlayOutline { int x,y,w,h; };
struct OverlayFrame {
    static constexpr unsigned capacity=32;
    OverlayCursor cursor;
    OverlayOutline outlines[capacity];
    unsigned count;
};

// One slot per private scanout. Store only inked cursor pixels. Offsets
// belong to that exact surface and are consumed before any new scene repair.
struct ScanoutCursorSave {
    struct Pixel { uint64_t offset; uint32_t color,ink; };
    Pixel pixels[CURSOR_SPRITE_SIZE*CURSOR_SPRITE_SIZE];
    unsigned count=0;
    uint8_t* surface=nullptr;
    int width=0,height=0,pitch=0;

    void paint(uint8_t* target,int w,int h,int stride,OverlayCursor cursor,const uint32_t* sprite){
        count=0;surface=target;width=w;height=h;pitch=stride;
        for(int y=0;y<CURSOR_SPRITE_SIZE;++y){
            int64_t dy=(int64_t)cursor.y-CURSOR_SPRITE_ORIGIN+y;
            if(dy<0||dy>=h)continue;
            for(int x=0;x<CURSOR_SPRITE_SIZE;++x){
                uint32_t color=sprite[y*CURSOR_SPRITE_SIZE+x];
                int64_t dx=(int64_t)cursor.x-CURSOR_SPRITE_ORIGIN+x;
                if(!color||dx<0||dx>=w)continue;
                uint64_t offset=(uint64_t)dy*stride+dx*4;
                auto* pixel=(volatile uint32_t*)(target+offset);
                pixels[count++]={offset,*pixel,color};
            }
        }
        // Complete WC reads before starting WC stores.
        for(unsigned i=0;i<count;++i)
            *(volatile uint32_t*)(target+pixels[i].offset)=pixels[i].ink;
    }
    bool restore(uint8_t* target,int w,int h,int stride){
        bool matches=surface&&surface==target&&width==w&&height==h&&pitch==stride;
        surface=nullptr;
        if(matches)for(unsigned i=0;i<count;++i)
            *(volatile uint32_t*)(target+pixels[i].offset)=pixels[i].color;
        count=0;
        return matches;
    }
};

// Half-open spans of a two-pixel outline, including all four corners.
template<class Span>
static void overlay_outline_spans(const OverlayOutline& r,int y,Span span) {
    if(r.w<=0||r.h<=0||y<r.y||y>=r.y+r.h)return;
    if(r.w<=4||y<r.y+2||y>=r.y+r.h-2)span(r.x,r.x+r.w);
    else {span(r.x,r.x+2);span(r.x+r.w-2,r.x+r.w);}
}

// Damage comes from the snapshots actually presented, independent of a window's
// current geometry or lifetime. The same snapshot is used for drawing/tracking.
template<class Present>
static void overlay_transition(const OverlayFrame& previous,const OverlayFrame& next,int height,Present present,bool scene_changed=false) {
    auto cursor=[&](const OverlayCursor& c){
        int top=c.y-CURSOR_SPRITE_ORIGIN,bottom=top+CURSOR_SPRITE_SIZE;
        if(top<0)top=0;if(bottom>height)bottom=height;
        for(int y=top;y<bottom;++y)present(c.x-CURSOR_SPRITE_ORIGIN,c.x+CURSOR_SPRITE_SIZE-CURSOR_SPRITE_ORIGIN,y);
    };
    // Place the new cursor first. Every following write composes this complete
    // next frame, preserving both the cursor and the new resize corner.
    bool cursor_changed=previous.cursor.x!=next.cursor.x||previous.cursor.y!=next.cursor.y||
        previous.cursor.edges!=next.cursor.edges||previous.cursor.buttons!=next.cursor.buttons;
    if(scene_changed||cursor_changed){cursor(next.cursor);cursor(previous.cursor);}
    auto outlines=[&](const OverlayFrame& f,const OverlayFrame& other){
        for(unsigned i=0;i<f.count;++i){const auto& r=f.outlines[i];
            if(!scene_changed&&i<other.count){const auto& old=other.outlines[i];
                if(r.x==old.x&&r.y==old.y&&r.w==old.w&&r.h==old.h)continue;}
            int top=r.y,bottom=r.y+r.h;
            if(top<0)top=0;if(bottom>height)bottom=height;
            for(int y=top;y<bottom;++y)overlay_outline_spans(r,y,[&](int left,int right){present(left,right,y);});
        }
    };
    outlines(previous,next);outlines(next,previous);
}

// Compose in bounded scratch memory before each framebuffer write. In
// particular, restoring old cursor/outline pixels must include the NEW outline
// already: a background-only erase followed by a draw visibly blinks corners.
// Caller clips [left,right) to the row. Source stays free of overlays.
template<class CursorColor,class Write>
static void overlay_present_row(uint32_t* destination,const uint32_t* source,
                                int left,int right,int y,const OverlayFrame& frame,
                                CursorColor cursor_color,Write write) {
    alignas(64) uint32_t pixels[256];
    const auto& cursor=frame.cursor;
    for(int x=left;x<right;){
        int end=x+256;if(end>right)end=right;
        bool cursor_row=y>=cursor.y-CURSOR_SPRITE_ORIGIN&&y<cursor.y+CURSOR_SPRITE_SIZE-CURSOR_SPRITE_ORIGIN;
        bool affected=cursor_row&&x<cursor.x+CURSOR_SPRITE_SIZE-CURSOR_SPRITE_ORIGIN&&end>cursor.x-CURSOR_SPRITE_ORIGIN;
        for(unsigned i=0;i<frame.count;++i)overlay_outline_spans(frame.outlines[i],y,[&](int a,int b){
            if(a<end&&b>x)affected=true;
        });
        if(!affected){write(destination+x,source+x,(end-x)*4);x=end;continue;}
        for(int px=x;px<end;++px)pixels[px-x]=source[px];
        for(unsigned i=0;i<frame.count;++i)overlay_outline_spans(frame.outlines[i],y,[&](int a,int b){
            if(a<x)a=x;if(b>end)b=end;
            for(int px=a;px<b;++px)pixels[px-x]=0xFFFFFFFF;
        });
        if(cursor_row){
            int a=cursor.x-CURSOR_SPRITE_ORIGIN,b=cursor.x+CURSOR_SPRITE_SIZE-CURSOR_SPRITE_ORIGIN;
            if(a<x)a=x;if(b>end)b=end;
            for(int px=a;px<b;++px){
                uint32_t color=cursor_color(cursor,px-cursor.x,y-cursor.y);
                if(color)pixels[px-x]=color;
            }
        }
        write(destination+x,pixels,(end-x)*4);
        x=end;
    }
}
#endif
