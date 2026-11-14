#include <cassert>
#include <initializer_list>
#include <cstdio>
#include "../src/gui/window_resize.h"
#include "../src/gui/window_decorations.h"
int main(){
    ResizeRect start{200,150,500,300};
    assert(resize_hit(start,30,201,121)==(RESIZE_LEFT|RESIZE_TOP));
    assert(resize_hit(start,30,698,448)==(RESIZE_RIGHT|RESIZE_BOTTOM));
    assert(resize_hit(start,30,202,250)==RESIZE_LEFT);
    assert(resize_hit(start,30,450,123)==RESIZE_TOP);
    assert(resize_hit(start,30,450,130)==0);assert(resize_hit(start,30,100,100)==0);
    auto left=resize_rect(start,RESIZE_LEFT,100,0,160,100,1280,730,30);
    assert(left.x==300&&left.w==400&&left.x+left.w==700);
    auto top=resize_rect(start,RESIZE_TOP,0,50,160,100,1280,730,30);
    assert(top.y==200&&top.h==250&&top.y+top.h==450);
    // Restoring a maximized drag preserves the grab fraction and saved size.
    for(int grab:{0,320,640,960,1279}){
        auto r=restore_drag_rect(start,1280,grab,15,640,100,30,1280);
        int expected=640-(int64_t)grab*start.w/1280;
        assert(r.x==expected&&r.y==115&&r.w==start.w&&r.h==start.h);
    }
    auto restore_left=restore_drag_rect(start,1280,1200,15,5,0,30,1280);
    assert(restore_left.x==0&&restore_left.y==30);
    auto restore_right=restore_drag_rect(start,1280,10,15,1279,100,30,1280);
    assert(restore_right.x==780&&restore_right.y==115);
    for(unsigned edges:{1u,2u,4u,8u,5u,6u,9u,10u})
        for(int dx=-1500;dx<=1500;dx+=31)for(int dy=-1000;dy<=1000;dy+=37){
            auto r=resize_rect(start,edges,dx,dy,160,100,1280,730,30);
            assert(r.x>=0&&r.y>=30&&r.x+r.w<=1280&&r.y+r.h<=760);
            assert(r.w>=160&&r.h>=100);
        }
    for(int width:{32,160,600})for(int title:{28,42,56})for(int control=0;control<3;++control){
        auto r=window_control_rect(100,80,width,title,control);
        assert(window_control_hit(100,80,width,title,r.x+r.w/2,r.y+r.h/2)==control);
        assert(r.x>=100&&r.x+r.w<=100+width&&r.y>=80&&r.y+r.h<=80+title);
    }
    for(int y=-7;y<=6;++y)for(int x=-7;x<=6;++x){
        bool ink=window_control_ink(WINDOW_CLOSE,false,x,y);
        assert(ink==window_control_ink(WINDOW_CLOSE,false,-1-x,y));
        assert(ink==window_control_ink(WINDOW_CLOSE,false,x,-1-y));
    }
    for(unsigned edges:{1u,2u,4u,8u,5u,6u,9u,10u})for(int y=-12;y<=12;++y)for(int x=-12;x<=12;++x){
        auto pixel=resize_cursor_pixel(edges,x,y);
        assert(pixel==resize_cursor_pixel(edges,-x,-y));
        if(x==-12||x==12||y==-12||y==12)assert(!pixel);
    }
    // Keep the arrows compact and their center shaft one light pixel wide.
    assert(resize_cursor_pixel(RESIZE_LEFT,0,0)==2);
    assert(resize_cursor_pixel(RESIZE_LEFT,0,1)==1);
    assert(resize_cursor_pixel(RESIZE_LEFT,0,2)==0);
    assert(resize_cursor_pixel(RESIZE_LEFT,10,0)==0);
    assert(resize_cursor_pixel(RESIZE_LEFT|RESIZE_TOP,8,8)==0);
    // Diagonal shafts have a single light pixel across, matching straight ones.
    for(unsigned edges:{5u,6u,9u,10u}){
        bool down=edges==5u||edges==10u;
        assert(resize_cursor_pixel(edges,3,down?3:-3)==2);
        assert(resize_cursor_pixel(edges,3,down?2:-2)==1);
        // A rotated triangular head has straight sides and a diagonal base:
        // scanlines widen by one pixel toward the tip, rather than forming a box.
        for(int y=1;y<=6;++y)for(int x=1;x<=6;++x)
            assert((resize_cursor_pixel(edges,x,down?y:-y)==2)==(x+y>=7||x==y));
        assert(resize_cursor_pixel(edges,6,down?6:-6)==2);
        assert(resize_cursor_pixel(edges,7,down?7:-7)==1);
        assert(resize_cursor_pixel(edges,8,down?8:-8)==0);
    }
    // Preview the actual compositor primitives at their physical pixel sizes.
    FILE* preview=std::fopen(".storage-test/window-decorations.ppm","wb");assert(preview);
    std::fprintf(preview,"P6\n240 100\n255\n");
    for(int y=0;y<100;++y)for(int x=0;x<240;++x){
        unsigned color=y<40?0x352a49:0x222131;
        for(int control=0;control<3;++control){auto r=window_control_rect(0,5,240,30,control);
            if(window_control_ink(control,false,x-(r.x+r.w/2),y-(r.y+r.h/2))&&x>=r.x&&x<r.x+r.w&&y>=r.y&&y<r.y+r.h)color=0xe9d7b0;}
        for(int i=0;i<4;++i){unsigned edges[]={1u,4u,5u,6u};
            unsigned pixel=resize_cursor_pixel(edges[i],x-(30+i*60),y-70);
            if(pixel)color=pixel==2?0xf8f3ff:0x17121e;}
        unsigned char rgb[]={(unsigned char)(color>>16),(unsigned char)(color>>8),(unsigned char)color};
        std::fwrite(rgb,1,3,preview);
    }
    std::fclose(preview);
}
