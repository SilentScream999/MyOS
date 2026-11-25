#ifndef WYZAR_VOLUME_POPUP_H
#define WYZAR_VOLUME_POPUP_H
struct VolumePopupLayout {
    int x,y,w,h;
    int slider_left() const { return x+64; }
    int slider_right() const { return x+w-24; }
    int slider_y() const { return y+h-34; }
    int percent(int pointer) const {
        int width=slider_right()-slider_left();
        if(pointer<=slider_left()||width<=0)return 0;
        if(pointer>=slider_right())return 100;
        return ((pointer-slider_left())*100+width/2)/width;
    }
    int thumb(unsigned volume) const {return slider_left()+(slider_right()-slider_left())*(int)volume/100;}
};
static VolumePopupLayout volume_popup_layout(int width,int height,int taskbar,int anchor) {
    int w=320;if(w>width-12)w=width-12;
    int h=124;if(h>height-taskbar-12)h=height-taskbar-12;
    int x=anchor+w>width-6?width-w-6:anchor;if(x<6)x=6;
    return {x,height-taskbar-h-4,w,h};
}
// Square 24px design: a tall speaker and separate, round-ended 90-degree arcs.
// Geometry uses eighths of a logical pixel, with 4x4 coverage per output pixel.
static unsigned volume_icon_coverage(unsigned percent,bool muted,int x,int y,int size) {
    if(size<=0)return 0;
    unsigned coverage=0;
    for(int sy=0;sy<4;++sy)for(int sx=0;sx<4;++sx){
        int px=(x*8+sx*2+1)*24/size,py=(y*8+sy*2+1)*24/size;
        bool ink=(px>=20&&px<=60&&py>=72&&py<=120)||
            (px>=60&&px<=100&&py>=132-px&&py<=60+px);
        if(muted||!percent){
            ink|=power_icon_stroke(px,py,124,72,172,120)||power_icon_stroke(px,py,172,72,124,120);
        }else{
            int dx=px-96,dy=py-96,distance=dx*dx+dy*dy;
            int ay=dy<0?-dy:dy;
            bool sector=dx>=ay;
            ink|=(sector&&distance>=48*48&&distance<=58*58)||
                ((dx-38)*(dx-38)+(ay-38)*(ay-38)<=25);
            if(percent>50)ink|=(sector&&distance>=76*76&&distance<=86*86)||
                ((dx-57)*(dx-57)+(ay-57)*(ay-57)<=25);
        }
        if(ink)++coverage;
    }
    return coverage;
}
#endif
