#include <cassert>
#include <algorithm>
#include <cstring>
#include <vector>
#define NATIVE_HOST_TEST
#include "../user/native_text.h"
#include "../user/shell_line.h"
static unsigned damages=0;
static uint64_t damage_x,damage_y,damage_w,damage_h;
static uint64_t native_call(uint64_t n,uint64_t,uint64_t x,uint64_t y,uint64_t w,uint64_t h){
    assert(n==204);++damages;damage_x=x;damage_y=y;damage_w=w;damage_h=h;return 0;
}
int main(){
    NativeTextCursor caret;
    caret.reset(10);assert(!caret.tick(39));assert(caret.tick(40)&&!caret.visible);
    assert(!caret.tick(69));assert(caret.tick(70)&&caret.visible);
    assert(caret.tick(71,false)&&!caret.visible);caret.reset(72);assert(caret.visible);
    // Unsigned frame wrap must preserve elapsed time.
    caret.reset(~0u-10);assert(caret.tick(19)&&!caret.visible);
    NativeTextInput input;char text[8]="abcd";bool replace=true;
    input.key(text,8,0x1044,replace,1);assert(!replace&&input.position==0);
    input.key(text,8,0x1043,replace,2);input.key(text,8,'X',replace,3);assert(!strcmp(text,"aXbcd"));
    input.key(text,8,8,replace,4);assert(!strcmp(text,"abcd"));
    input.key(text,8,0x1102,replace,5);assert(!strcmp(text,"acd"));
    assert(input.insert(text,8,"YZ",2,replace,6)&&!strcmp(text,"aYZcd"));
    assert(!input.insert(text,8,"XYZ",3,replace,7)&&!strcmp(text,"aYZcd"));
    replace=true;input.key(text,8,0x1043,replace,8);assert(input.position==5);
    replace=true;assert(input.insert(text,8,"ok",2,replace,9)&&!strcmp(text,"ok"));
    std::vector<uint32_t> pixels(40*24);NativeSurface surface{pixels.data(),40,24};
    char long_text[]="a very long input";replace=true;input.draw(surface,4,2,36,long_text,replace);
    assert(input.first>0);input.key(long_text,sizeof(long_text),0x1100,replace,10);
    input.draw(surface,4,2,36,long_text,replace);assert(input.first==0&&input.position==0);
    char command[8]="abcd";int length=4,position=2;
    assert(shell_line_insert(command,8,&length,&position,'X')&&!strcmp(command,"abXcd"));
    assert(shell_line_backspace(command,&length,&position)&&!strcmp(command,"abcd"));
    position=0;assert(!shell_line_backspace(command,&length,&position));
    for(int i=0;i<3;++i)assert(shell_line_insert(command,8,&length,&position,'z'));
    assert(!shell_line_insert(command,8,&length,&position,'!')&&command[7]==0);
    assert(!shell_line_insert(command,8,&length,&position,'\n'));
    // Blinking restores the exact glyph/background pixels and submits only the
    // caret column. Unchanged frames do no presentation work.
    for(unsigned i=0;i<pixels.size();++i)pixels[i]=0xff000000|i;
    auto original=pixels;NativeTextCursor blinking;
    assert(!blinking.animate(surface,1,0,true,true));blinking.draw(surface,10,2,14);
    auto shown=pixels;
    for(unsigned f=1;f<30;++f)assert(!blinking.animate(surface,1,f,true,false));
    assert(damages==0&&pixels==shown);
    assert(!blinking.animate(surface,1,30,true,false));
    assert(damages==1&&pixels==original&&damage_x==10&&damage_y==2&&damage_w==1&&damage_h==14);
    assert(!blinking.animate(surface,1,60,true,false));assert(damages==2&&pixels==shown);
    assert(!blinking.animate(surface,1,61,false,false));assert(damages==3&&pixels==original);
    assert(!blinking.animate(surface,1,100,false,false));assert(damages==3);
    // A full repaint can replace the covered content while the caret is off.
    blinking.reset(100);blinking.animate(surface,1,100,true,true);
    std::fill(pixels.begin(),pixels.end(),0xff123456);blinking.draw(surface,5,1,19);
    auto before_repaint=damages;
    assert(!blinking.animate(surface,1,130,true,true));assert(damages==before_repaint);
    std::fill(pixels.begin(),pixels.end(),0xffabcdef);blinking.draw(surface,5,1,19);
    assert(!blinking.animate(surface,1,160,true,false));
    assert(!blinking.animate(surface,1,190,true,false));
    for(auto p:pixels)assert(p==0xffabcdef);
    // Resizing/replacing the surface must never restore stale pixels into it.
    std::vector<uint32_t> resized(80*30,0xff777777);surface={resized.data(),80,30};
    assert(blinking.animate(surface,1,220,true,false));
    for(auto p:resized)assert(p==0xff777777);
}
