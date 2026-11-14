#ifndef WYZAR_WINDOW_DECORATIONS_H
#define WYZAR_WINDOW_DECORATIONS_H
#include "window_resize.h"

enum { WINDOW_CLOSE=0, WINDOW_MAXIMIZE=1, WINDOW_MINIMIZE=2, WINDOW_NO_CONTROL=-1 };
struct WindowControlRect { int x,y,w,h; };
static WindowControlRect window_control_rect(int x,int title_y,int width,int title,int control) {
    int pad=title/10;if(pad<2)pad=2;
    int height=title-2*pad,button_width=title+8;
    int available=(width-2*pad-4)/3;if(button_width>available)button_width=available;
    return {x+width-pad-(control+1)*button_width-control*2,title_y+pad,button_width,height};
}
static int window_control_hit(int x,int title_y,int width,int title,int mx,int my) {
    for(int control=0;control<3;++control){auto r=window_control_rect(x,title_y,width,title,control);
        if(mx>=r.x&&mx<r.x+r.w&&my>=r.y&&my<r.y+r.h)return control;}
    return WINDOW_NO_CONTROL;
}
// Centered geometric symbols, independent of font metrics. Coordinates are
// relative to the symbol center; every stroke is two physical pixels thick.
static bool window_control_ink(int control,bool maximized,int x,int y) {
    // A 12-pixel square gives both diagonals complete, matching end caps.
    // Mirror around the half-pixel center so neither side loses a stroke pixel.
    if(control==WINDOW_CLOSE){int a=x-y,b=x+y+1;
        return x>=-6&&x<=5&&y>=-6&&y<=5&&((a>=-1&&a<=1)||(b>=-1&&b<=1));}
    if(control==WINDOW_MINIMIZE)return x>=-5&&x<=5&&y>=3&&y<=4;
    auto box=[](int px,int py,int left,int top,int right,int bottom){
        return px>=left&&px<=right&&py>=top&&py<=bottom&&
            (px<left+2||px>right-2||py<top+2||py>bottom-2);};
    if(!maximized)return box(x,y,-5,-5,5,5);
    return box(x,y,-5,-2,2,5)||(box(x,y,-2,-5,5,2)&&(y<-2||x>2));
}
static bool resize_arrow_ink(int x,int y) {
    if(x<0)x=-x;if(y<0)y=-y;
    return (x<=6&&y==0)||(x>=5&&x<=8&&y<=8-x);
}
// 0 = transparent, 1 = dark outline, 2 = light fill.
static unsigned resize_arrow_pixel(int x,int y) {
    if(resize_arrow_ink(x,y))return 2;
    for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx)
        if(resize_arrow_ink(x+dx,y+dy))return 1;
    return 0;
}
static unsigned resize_cursor_pixel(unsigned edges,int x,int y) {
    bool horizontal=edges&(RESIZE_LEFT|RESIZE_RIGHT),vertical=edges&(RESIZE_TOP|RESIZE_BOTTOM);
    if(horizontal&&vertical){
        bool down=edges==(RESIZE_LEFT|RESIZE_TOP)||edges==(RESIZE_RIGHT|RESIZE_BOTTOM);
        int dy=down?y:-y;
        // Rotate the arrow geometry before rasterizing, rather than rotating
        // rounded bitmap samples (which turn the small triangle into a box).
        // 181/256 approximates 1/sqrt(2); coordinates retain subpixel precision.
        int u=(x+dy)*181,v=(x-dy)*181;
        if(u<0)u=-u;if(v<0)v=-v;
        bool shaft=u<=1664&&v<=128; // half-length 6.5, half-width 0.5
        bool head=u>=1152&&u+v<=2176; // triangle from 4.5 to 8.5
        if(shaft||head)return 2;
        // Offset the same shapes by one pixel for a consistent dark border.
        bool shaft_border=u<=1920&&v<=384;
        bool head_border=u>=896&&u+v<=2538;
        return shaft_border||head_border?1:0;
    }
    return vertical?resize_arrow_pixel(y,x):resize_arrow_pixel(x,y);
}
#endif
