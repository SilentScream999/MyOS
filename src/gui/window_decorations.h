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
static bool resize_cursor_ink(unsigned edges,int x,int y) {
    bool horizontal=edges&(RESIZE_LEFT|RESIZE_RIGHT),vertical=edges&(RESIZE_TOP|RESIZE_BOTTOM);
    int u=x,v=y,tip=10,base=5,shaft=1;
    if(horizontal&&vertical){
        bool down=edges==(RESIZE_LEFT|RESIZE_TOP)||edges==(RESIZE_RIGHT|RESIZE_BOTTOM);
        u=x+(down?y:-y);v=x-(down?y:-y);tip=16;base=8;shaft=2;
    }else if(vertical){u=y;v=x;}
    if(u<0)u=-u;if(v<0)v=-v;
    return (u<=base+1&&v<=shaft)||(u>=base&&u<=tip&&v<=tip-u);
}
// 0 = transparent, 1 = dark outline, 2 = light fill.
static unsigned resize_cursor_pixel(unsigned edges,int x,int y) {
    if(resize_cursor_ink(edges,x,y))return 2;
    for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx)
        if(resize_cursor_ink(edges,x+dx,y+dy))return 1;
    return 0;
}
#endif
