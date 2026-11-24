#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "src/gui/title_bar.h"
#include "src/gui/window_decorations.h"

int main() {
    uint32_t icon_pixels[16*16];for(auto& pixel:icon_pixels)pixel=0x80402010;
    TitleBarKey key{};key.width=240;key.height=28;key.font=ui_font_for_pixels(14);
    key.icon={16,16,icon_pixels};key.resource_epoch=1;key.closable=true;key.focused=true;
    key.hover=WINDOW_NO_CONTROL;std::strcpy(key.title,"A title with an icon");
    auto equal=key;assert(title_bar_equal(key,equal));
    auto changed=[&](auto mutate){auto copy=key;mutate(copy);assert(!title_bar_equal(key,copy));};
    changed([](auto& k){++k.width;});changed([](auto& k){++k.height;});
    changed([](auto& k){k.focused=false;});changed([](auto& k){k.maximized=true;});
    changed([](auto& k){k.closable=false;});changed([](auto& k){k.hover=WINDOW_CLOSE;});
    changed([](auto& k){k.title[0]='Z';});changed([](auto& k){++k.resource_epoch;});
    changed([](auto& k){k.icon.pixels=nullptr;});changed([](auto& k){++k.icon.width;});
    changed([](auto& k){k.font=ui_font_for_pixels(24);});
    std::vector<uint32_t> cached(key.width*key.height);
    title_bar_render(key,{cached.data(),key.width,key.height,key.width},0,0,{0,0,key.width,key.height});
    assert(cached[0]==0xff77608f);
    assert(cached[27*key.width+2]==0xff352a49);
    // Premultiplied icon over the focused bar (rounded source-over).
    assert(cached[8*key.width+8]==0xff5a3534);
    bool title_ink=false;for(int y=4;y<24;++y)for(int x=30;x<100;++x)
        if(cached[y*key.width+x]!=0xff352a49)title_ink=true;
    assert(title_ink);
    auto close=window_control_rect(0,0,key.width,key.height,WINDOW_CLOSE);
    key.hover=WINDOW_CLOSE;
    std::vector<uint32_t> hovered(cached.size());
    title_bar_render(key,{hovered.data(),key.width,key.height,key.width},0,0,{0,0,key.width,key.height});
    assert(hovered[(close.y+1)*key.width+close.x+1]==0xff9a405d);
    key.hover=WINDOW_NO_CONTROL;
    // Cache blits and direct fallback must agree for partial/offscreen titles,
    // preserving every pixel and padding word outside the exclusive clip.
    constexpr int width=260,height=80,stride=272;constexpr uint32_t guard=0x12345678;
    for(int n=0;n<36;++n){
        int x=-40+n*3,y=-12+n;VisibleRect clip{17,9,201,67};
        std::vector<uint32_t> direct(stride*height,guard),blitted=direct;
        title_bar_render(key,{direct.data(),width,height,stride},x,y,clip);
        title_bar_blit(cached.data(),key.width,key.height,{blitted.data(),width,height,stride},x,y,clip);
        assert(direct==blitted);
        for(int py=0;py<height;++py)for(int px=0;px<stride;++px)
            if(px<clip.x1||px>=clip.x2||py<clip.y1||py>=clip.y2)assert(direct[py*stride+px]==guard);
    }
    puts("Title-bar cache keys, text/icon/control pixels, offscreen blits and partial fallback passed.");
}
