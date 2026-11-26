#include <cassert>
#include <initializer_list>
#include "src/gui/power_icons.h"
#include "src/gui/volume_popup.h"
#include "src/drivers/audio_volume.h"
int main(){
    for(auto screen:{320,640,1280,1920}){
        auto panel=volume_popup_layout(screen,screen*3/4,40,screen-100);
        assert(panel.x>=6&&panel.x+panel.w<=screen-6&&panel.y>=0);
        assert(panel.slider_right()>panel.slider_left());
        assert(panel.percent(panel.slider_left()-500)==0&&panel.percent(panel.slider_right()+500)==100);
        for(unsigned p=0;p<=100;++p){int actual=panel.percent(panel.thumb(p));assert(actual>=(int)p-1&&actual<=(int)p+1);}
    }
    unsigned loud=0,muted=0,quiet=0;
    for(int y=0;y<24;++y)for(int x=0;x<24;++x){
        auto a=volume_icon_coverage(80,false,x,y,24),b=volume_icon_coverage(80,true,x,y,24),c=volume_icon_coverage(10,false,x,y,24);
        assert(a<=16&&b<=16&&c<=16);loud+=a;muted+=b;quiet+=c;
    }
    assert(loud&&muted&&quiet&&loud!=muted&&loud>quiet);
    for(int size:{12,16,18,20,22}){
        unsigned ink=0;
        for(int y=0;y<size;++y)for(int x=0;x<size;++x){auto a=volume_icon_coverage(80,false,x,y,size);assert(a<=16);ink+=a;}
        assert(ink>0);
    }
    AudioMixer mixer{};
    mixer.percent(200);assert(mixer.value.percent==100&&mixer.revision==2);
    mixer.percent(100);assert(mixer.revision==2);
    mixer.mute(2);assert(mixer.value.muted);
    mixer.key(0x2e);assert(!mixer.value.muted&&mixer.value.percent==90);
}
