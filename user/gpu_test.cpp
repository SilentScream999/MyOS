#include "native_ui.h"

static constexpr int WIDTH=320,HEIGHT=240;

static void draw(NativeSurface& surface,unsigned mode,bool animate,unsigned frame) {
    const uint32_t colors[]={0xFF235789,0xFFF1D302,0xFFDE3C4B,0xFF2B9E77};
    for(int y=0;y<HEIGHT;++y)for(int x=0;x<WIDTH;++x){
        uint32_t color=colors[0];
        if(mode==2)color=colors[((x/32)+(y/32))%4];
        if(mode==3)color=0xFF000040u|((unsigned)x*255/(WIDTH-1)<<16)|
                                          ((unsigned)y*255/(HEIGHT-1)<<8);
        surface.pixels[y*WIDTH+x]=color;
    }
    surface.rect(0,0,WIDTH,2,0xFFFFFFFF);
    surface.rect(0,HEIGHT-2,WIDTH,2,0xFFFFFFFF);
    surface.rect(0,0,2,HEIGHT,0xFFFFFFFF);
    surface.rect(WIDTH-2,0,2,HEIGHT,0xFFFFFFFF);
    if(animate)surface.rect(4+(frame*3)%(WIDTH-28),70,24,24,0xFFFFFFFF);
    surface.rect(8,166,WIDTH-16,64,0xFF18202A);
    surface.text(16,172,"1 Solid   2 Grid   3 Gradient",0xFFFFFFFF,WIDTH-16,12);
    surface.text(16,190,animate?"Space pauses animation":"Space starts animation",0xFFFFFFFF,WIDTH-16,12);
    surface.text(16,208,"Resize or maximize this window",0xFFD0D6DD,WIDTH-16,12);
}

extern "C" void _start() {
    const char* title="GPU Scaling Test";
    uint64_t window=native_call(200,(uint64_t)title,160,120,WIDTH,HEIGHT);
    if(!window){native_call(60,1);for(;;){}}
    auto* pixels=(uint32_t*)native_call(203,window);
    if(!pixels){native_call(60,1);for(;;){}}
    NativeSurface surface{pixels,WIDTH,HEIGHT};
    unsigned mode=2,frame=0;
    bool animate=false,dirty=true;
    for(;;){
        NativeEvent event{};
        while(native_call(205,window,(uint64_t)&event)){
            if(event.type==7){native_call(60,0);for(;;){}}
            if(event.type!=5)continue;
            if(event.key>='1'&&event.key<='3'){mode=event.key-'0';dirty=true;}
            if(event.key==' '){animate=!animate;dirty=true;}
        }
        if(dirty||animate){
            draw(surface,mode,animate,frame);
            dirty=(int64_t)native_call(222,window,0)<0;
            if(!dirty)++frame;
        }
        native_call(35,33);
    }
}
