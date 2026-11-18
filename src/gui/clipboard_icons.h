#ifndef WYZAR_CLIPBOARD_ICONS_H
#define WYZAR_CLIPBOARD_ICONS_H
#include "power_icons.h"

enum class ClipboardIcon { Pin, Pinned, Delete };

static unsigned clipboard_icon_coverage(ClipboardIcon icon,int x,int y,int size){
    unsigned coverage=0;
    for(int sy=0;sy<4;++sy)for(int sx=0;sx<4;++sx){
        int px=(x*8+sx*2+1)*24/size-96;
        int py=(y*8+sy*2+1)*24/size-96;
        bool ink;
        if(icon==ClipboardIcon::Delete){
            auto stroke=[&](int a,int b,int c,int d){return power_icon_stroke(px,py,a,b,c,d);};
            ink=stroke(-57,-39,57,-39)||stroke(-17,-39,-17,-60)||
                stroke(-17,-60,17,-60)||stroke(17,-60,17,-39)||
                stroke(-43,-24,-35,54)||stroke(-35,54,35,54)||stroke(35,54,43,-24)||
                stroke(-13,-14,-11,33)||stroke(13,-14,11,33);
        }else{
            // Rotate an upright pushpin by 45 degrees toward the lower left.
            int rx=(px+py)*181/256,ry=(py-px)*181/256;
            auto stroke=[&](int a,int b,int c,int d){return power_icon_stroke(rx,ry,a,b,c,d);};
            ink=stroke(-37,-63,37,-63)||stroke(-37,-63,-37,-44)||
                stroke(37,-63,37,-44)||stroke(-37,-44,-21,-44)||stroke(21,-44,37,-44)||
                stroke(-21,-44,-21,-12)||stroke(21,-44,21,-12)||
                stroke(-21,-12,-46,10)||stroke(21,-12,46,10)||
                stroke(-46,10,-46,21)||stroke(46,10,46,21)||stroke(-46,21,46,21)||
                stroke(0,21,0,70);
            if(icon==ClipboardIcon::Pinned){
                int ax=rx<0?-rx:rx;
                ink|=(ry>=-63&&ry<=-44&&ax<=37)||(ry>=-44&&ry<=-12&&ax<=21)||
                     (ry>=-12&&ry<=10&&ax<=21+(ry+12)*25/22)||(ry>=10&&ry<=21&&ax<=46);
            }
        }
        if(ink)++coverage;
    }
    return coverage;
}
#endif
