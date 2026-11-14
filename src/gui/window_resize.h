#ifndef WYZAR_WINDOW_RESIZE_H
#define WYZAR_WINDOW_RESIZE_H
#include <stdint.h>
enum { RESIZE_LEFT=1,RESIZE_RIGHT=2,RESIZE_TOP=4,RESIZE_BOTTOM=8 };
struct ResizeRect { int32_t x,y,w,h; };
static unsigned resize_hit(const ResizeRect& r,int title,int x,int y,int grip=6) {
    if(x<r.x||x>=r.x+r.w||y<r.y-title||y>=r.y+r.h)return 0;
    unsigned edges=0;
    if(x<r.x+grip)edges|=RESIZE_LEFT;else if(x>=r.x+r.w-grip)edges|=RESIZE_RIGHT;
    if(y<r.y-title+grip)edges|=RESIZE_TOP;else if(y>=r.y+r.h-grip)edges|=RESIZE_BOTTOM;
    return edges;
}
static ResizeRect resize_rect(ResizeRect original,unsigned edges,int dx,int dy,int minw,int minh,int maxw,int maxh,int title) {
    ResizeRect r=original;
    int right=original.x+original.w,bottom=original.y+original.h;
    if(edges&RESIZE_LEFT){r.x+=dx;r.w-=dx;}else if(edges&RESIZE_RIGHT)r.w+=dx;
    if(edges&RESIZE_TOP){r.y+=dy;r.h-=dy;}else if(edges&RESIZE_BOTTOM)r.h+=dy;
    if(minw>maxw)minw=maxw;if(minh>maxh)minh=maxh;
    if(r.w<minw)r.w=minw;if(r.w>maxw)r.w=maxw;
    if(r.h<minh)r.h=minh;if(r.h>maxh)r.h=maxh;
    if(edges&RESIZE_LEFT)r.x=right-r.w;if(edges&RESIZE_TOP)r.y=bottom-r.h;
    if(r.x<0){if(edges&RESIZE_LEFT)r.w+=r.x;r.x=0;}
    if(r.y<title){if(edges&RESIZE_TOP)r.h-=title-r.y;r.y=title;}
    if(r.x+r.w>maxw){if(edges&RESIZE_RIGHT)r.w=maxw-r.x;else r.x=maxw-r.w;}
    if(r.y+r.h>title+maxh){if(edges&RESIZE_BOTTOM)r.h=title+maxh-r.y;else r.y=title+maxh-r.h;}
    return r;
}
#endif
