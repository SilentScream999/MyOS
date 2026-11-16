#ifndef WYZAR_POWER_ICONS_H
#define WYZAR_POWER_ICONS_H

// Geometric strokes in eighths of a logical pixel, sampled at 4x4 per pixel.
// Integer arithmetic keeps these small outline icons independent of FPU state.
static bool power_icon_stroke(int x,int y,int ax,int ay,int bx,int by) {
    int dx=bx-ax,dy=by-ay,px=x-ax,py=y-ay;
    int length=dx*dx+dy*dy,dot=px*dx+py*dy;
    const int radius=6;
    if(dot<=0)return px*px+py*py<=radius*radius;
    if(dot>=length){px=x-bx;py=y-by;return px*px+py*py<=radius*radius;}
    int cross=px*dy-py*dx;
    return (long long)cross*cross<=(long long)radius*radius*length;
}
static unsigned power_icon_coverage(bool shutdown,int x,int y,int size) {
    if(size<=0)return 0;
    unsigned coverage=0;
    for(int sy=0;sy<4;++sy)for(int sx=0;sx<4;++sx) {
        int px=(x*8+sx*2+1)*24/size-96;
        int py=(y*8+sy*2+1)*24/size-96;
        int distance=px*px+py*py;
        bool ring=distance>=50*50&&distance<=62*62;
        bool ink;
        if(shutdown) {
            ink=(ring&&!(py<-24&&px>-32&&px<32))||
                power_icon_stroke(px,py,0,-64,0,-8);
        } else {
            ink=(ring&&!(px>0&&py<0))||
                power_icon_stroke(px,py,0,-56,56,-56)||
                power_icon_stroke(px,py,32,-80,56,-56)||
                power_icon_stroke(px,py,56,-56,32,-32);
        }
        if(ink)++coverage;
    }
    return coverage;
}
#endif
