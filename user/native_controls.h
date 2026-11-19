#ifndef WYZAR_NATIVE_CONTROLS_H
#define WYZAR_NATIVE_CONTROLS_H
#include "native_ui.h"
#include "src/gui/power_icons.h"

enum class NativeIcon {
    Up, Refresh, Open, Minus, Plus, Fit, ActualSize, Back, Forward,
    NewFolder, NewFile, Rename, Copy, Cut, Paste, Delete, Install, Pin, ShowHidden, Eject, DisclosureDown
};

// Use the Start menu's restart artwork for refresh. Other toolbar symbols use
// the same stroke width and integer supersampling, without font glyphs/assets.
static unsigned native_icon_coverage(NativeIcon icon,int x,int y,int size){
    if(icon==NativeIcon::Refresh)return power_icon_coverage(false,x,y,size);
    unsigned coverage=0;
    for(int sy=0;sy<4;++sy)for(int sx=0;sx<4;++sx){
        int px=(x*8+sx*2+1)*24/size-96,py=(y*8+sy*2+1)*24/size-96;
        auto stroke=[&](int a,int b,int c,int d){return power_icon_stroke(px,py,a,b,c,d);};
        auto ring=[&](int cx,int cy,int radius){
            int dx=px-cx,dy=py-cy,distance=dx*dx+dy*dy;
            return distance>=(radius-6)*(radius-6)&&distance<=(radius+6)*(radius+6);
        };
        bool ink=false;
        if(icon==NativeIcon::Up)ink=stroke(0,56,0,-48)||stroke(-40,-8,0,-48)||stroke(0,-48,40,-8);
        else if(icon==NativeIcon::Back)ink=stroke(32,-48,-16,0)||stroke(-16,0,32,48);
        else if(icon==NativeIcon::Forward)ink=stroke(-32,-48,16,0)||stroke(16,0,-32,48);
        else if(icon==NativeIcon::Open)ink=stroke(-64,-32,-24,-32)||stroke(-24,-32,-8,-16)||
            stroke(-8,-16,64,-16)||stroke(64,-16,48,56)||stroke(48,56,-64,56)||
            stroke(-64,56,-64,-32)||stroke(-64,8,56,8);
        else if(icon==NativeIcon::Minus||icon==NativeIcon::Plus)
            ink=stroke(-48,0,48,0)||(icon==NativeIcon::Plus&&stroke(0,-48,0,48));
        else if(icon==NativeIcon::Fit)ink=
            stroke(-56,-16,-56,-48)||stroke(-56,-48,-24,-48)||
            stroke(24,-48,56,-48)||stroke(56,-48,56,-16)||
            stroke(-56,16,-56,48)||stroke(-56,48,-24,48)||
            stroke(24,48,56,48)||stroke(56,48,56,16)||
            stroke(-24,-16,24,-16)||stroke(24,-16,24,16)||
            stroke(24,16,-24,16)||stroke(-24,16,-24,-16);
        else if(icon==NativeIcon::ActualSize)ink=stroke(-52,-52,52,-52)||stroke(52,-52,52,52)||
            stroke(52,52,-52,52)||stroke(-52,52,-52,-52)||(px>=-12&&px<=12&&py>=-12&&py<=12);
        else if(icon==NativeIcon::NewFolder)ink=
            stroke(-64,-24,-64,-56)||stroke(-64,-56,-20,-56)||stroke(-20,-56,8,-24)||
            stroke(8,-24,64,-24)||stroke(64,-24,64,56)||stroke(64,56,-64,56)||
            stroke(-64,56,-64,-24)||stroke(-64,-8,64,-8)||stroke(-12,24,28,24)||stroke(8,4,8,44);
        else if(icon==NativeIcon::NewFile)ink=
            stroke(-48,-64,16,-64)||stroke(16,-64,48,-32)||stroke(48,-32,48,64)||
            stroke(48,64,-48,64)||stroke(-48,64,-48,-64)||stroke(16,-64,16,-32)||
            stroke(16,-32,48,-32)||stroke(-28,20,12,20)||stroke(-8,0,-8,40);
        else if(icon==NativeIcon::Rename)ink=
            stroke(-56,56,-48,28)||stroke(-48,28,36,-56)||stroke(36,-56,56,-36)||
            stroke(56,-36,-28,48)||stroke(-28,48,-56,56)||
            stroke(-48,28,-28,48)||stroke(20,-40,40,-20);
        else if(icon==NativeIcon::Copy)ink=
            stroke(-64,24,-64,-64)||stroke(-64,-64,24,-64)||stroke(24,-64,24,-40)||
            stroke(-64,24,-40,24)||stroke(-32,-32,64,-32)||stroke(64,-32,64,64)||
            stroke(64,64,-32,64)||stroke(-32,64,-32,-32);
        else if(icon==NativeIcon::Cut)ink=ring(-40,40,24)||ring(40,40,24)||
            stroke(-24,20,52,-64)||stroke(24,20,-52,-64);
        else if(icon==NativeIcon::Paste)ink=
            stroke(-52,-48,-24,-48)||stroke(24,-48,52,-48)||stroke(52,-48,52,64)||
            stroke(52,64,-52,64)||stroke(-52,64,-52,-48)||stroke(-24,-64,24,-64)||
            stroke(24,-64,24,-28)||stroke(24,-28,-24,-28)||stroke(-24,-28,-24,-64)||
            stroke(-24,4,24,4)||stroke(-24,32,24,32);
        else if(icon==NativeIcon::Delete)ink=
            stroke(-64,-40,64,-40)||stroke(-24,-40,-24,-64)||stroke(-24,-64,24,-64)||
            stroke(24,-64,24,-40)||stroke(-48,-16,-40,64)||stroke(-40,64,40,64)||
            stroke(40,64,48,-16)||stroke(-16,0,-16,40)||stroke(16,0,16,40);
        else if(icon==NativeIcon::Install)ink=
            stroke(0,-64,0,24)||stroke(-32,-8,0,24)||stroke(0,24,32,-8)||
            stroke(-64,24,-64,64)||stroke(-64,64,64,64)||stroke(64,64,64,24);
        else if(icon==NativeIcon::Pin)ink=
            stroke(-36,-64,36,-64)||stroke(-24,-64,-24,-12)||stroke(-24,-12,-52,16)||
            stroke(-52,16,52,16)||stroke(52,16,24,-12)||stroke(24,-12,24,-64)||stroke(0,16,0,72);
        else if(icon==NativeIcon::ShowHidden)ink=
            stroke(-72,0,-48,-28)||stroke(-48,-28,0,-44)||stroke(0,-44,48,-28)||stroke(48,-28,72,0)||
            stroke(72,0,48,28)||stroke(48,28,0,44)||stroke(0,44,-48,28)||stroke(-48,28,-72,0)||
            px*px+py*py<=18*18;
        else if(icon==NativeIcon::Eject){
            int distance=px<0?-px:px;
            ink=(py>=-56&&py<=20&&distance*76<=(py+56)*56)||
                (px>=-56&&px<=56&&py>=44&&py<=60);
        }
        else if(icon==NativeIcon::DisclosureDown)ink=stroke(-48,-24,0,24)||stroke(0,24,48,-24);
        if(ink)++coverage;
    }
    return coverage;
}
static void native_draw_icon(NativeSurface& surface,NativeIcon icon,int x,int y,int size,uint32_t color){
    for(int py=0;py<size;++py)for(int px=0;px<size;++px){
        if(x+px<0||x+px>=surface.w||y+py<0||y+py>=surface.h)continue;
        unsigned coverage=native_icon_coverage(icon,px,py,size);
        if(coverage){auto& pixel=surface.pixels[(y+py)*surface.w+x+px];
            pixel=ui_font_blend(pixel,color,(uint8_t)((coverage*255+8)/16));}
    }
}
struct NativeToolbarButton {
    int x,y,w,h;NativeIcon icon;const char* label;const char* hint;
    bool hit(int px,int py)const{return px>=x&&px<x+w&&py>=y&&py<y+h;}
    void draw(NativeSurface& surface,bool enabled=true,bool hover=false,bool active=false)const{
        hover=hover&&enabled;active=active&&enabled;
        uint32_t edge=active?0xff9971cb:hover?0xff87729f:0xff41394f;
        uint32_t fill=active?0xff44315f:hover?0xff3b304e:0xff2c273b;
        uint32_t ink=!enabled?0xff756d85:active?0xffe3c9ff:0xffeeebf8;
        surface.rounded(x,y,w,h,6,edge);surface.rounded(x+1,y+1,w-2,h-2,5,fill);
        int text_width=label?ui_text_width(ui_font_for_pixels(12),label):0;
        int size=18,start=x+(w-size-(text_width?text_width+5:0))/2;
        native_draw_icon(surface,icon,start,y+(h-size)/2,size,ink);
        if(text_width)surface.text(start+size+5,y+(h-ui_font_for_pixels(12)->height)/2,label,ink,x+w-4,12);
    }
};
#endif
