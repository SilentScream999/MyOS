#ifndef WYZAR_NATIVE_UI_H
#define WYZAR_NATIVE_UI_H
#include <stdint.h>
#include "src/fs/fileinfo.h"
#include "src/gui/ui_font.h"

struct NativeEvent { uint32_t type; int32_t x, y; uint32_t key, mods; int32_t delta; };
#ifndef NATIVE_HOST_TEST
static uint64_t native_call(uint64_t n, uint64_t a=0, uint64_t b=0, uint64_t c=0,
                            uint64_t d=0, uint64_t e=0) {
    register uint64_t r10 __asm__("r10")=d, r8 __asm__("r8")=e;
    uint64_t result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return result;
}
extern "C" void* memcpy(void* dst, const void* src, unsigned long n) {
    auto* d=(uint8_t*)dst; auto* s=(const uint8_t*)src;
    for (unsigned long i=0;i<n;++i) d[i]=s[i]; return dst;
}
extern "C" void* memset(void* dst, int value, unsigned long n) {
    auto* d=(uint8_t*)dst; for (unsigned long i=0;i<n;++i) d[i]=(uint8_t)value; return dst;
}
#else
static uint64_t native_call(uint64_t, uint64_t=0, uint64_t=0, uint64_t=0, uint64_t=0, uint64_t=0);
#endif
static uint64_t sys_open(const char* p,uint64_t f) { return native_call(2,(uint64_t)p,f); }
static void sys_close(uint64_t f) { native_call(3,f); }
static uint64_t sys_read(uint64_t f,char* b,uint64_t n) { return native_call(0,f,(uint64_t)b,n); }
static uint64_t sys_write(uint64_t f,const char* b,uint64_t n) { return native_call(1,f,(uint64_t)b,n); }
static int64_t sys_lseek(uint64_t f,int64_t n,uint64_t w) { return native_call(8,f,n,w); }
static int64_t sys_stat(const char* p,file_info* s) { return native_call(4,(uint64_t)p,(uint64_t)s); }
static int64_t sys_mkdir(const char* p,uint64_t m) { return native_call(83,(uint64_t)p,m); }
static int64_t sys_rmdir(const char* p) { return native_call(84,(uint64_t)p); }
static int64_t sys_unlink(const char* p) { return native_call(87,(uint64_t)p); }
static int64_t sys_rename(const char* p,const char* q) { return native_call(82,(uint64_t)p,(uint64_t)q); }
static int64_t sys_exchange(const char* p,const char* q) { return native_call(211,(uint64_t)p,(uint64_t)q); }
static int64_t sys_replace(const char* p,const void* b,uint64_t n) { return native_call(138,(uint64_t)p,(uint64_t)b,n); }
static int64_t sys_readdir(const char* p,uint64_t i,char* n,uint64_t* s,uint64_t* t) {
    return native_call(40,(uint64_t)p,i,(uint64_t)n,(uint64_t)s,(uint64_t)t);
}
static unsigned native_length(const char* s) { unsigned n=0; while(s[n]) ++n; return n; }
static bool native_copy(char* d,unsigned cap,const char* s) {
    unsigned n=native_length(s); if(n>=cap) return false;
    for(unsigned i=0;i<=n;++i) d[i]=s[i]; return true;
}
static void native_number(uint64_t value,char* out) {
    char digits[24]; unsigned n=0,k=0;
    do { digits[n++]=(char)('0'+value%10); value/=10; } while(value);
    while(n) out[k++]=digits[--n]; out[k]=0;
}
struct NativeSurface {
    uint32_t* pixels; int w,h;
    struct Point { int x,y; };
    void rect(int x,int y,int width,int height,uint32_t color) {
        int r=x+width,b=y+height; if(x<0)x=0;if(y<0)y=0;if(r>w)r=w;if(b>h)b=h;
        for(int py=y;py<b;++py) for(int px=x;px<r;++px) pixels[py*w+px]=color;
    }
    void text(int x,int y,const char* value,uint32_t color,int right=0,int size=14) {
        ui_text_draw(pixels,w,h,w,ui_font_for_pixels(size),x,y,value,color,0,0,right?right:w,h);
    }
    void centered_text(int x,int y,int width,int height,const char* value,uint32_t color,int size=14) {
        const UiFont* font=ui_font_for_pixels(size);
        text(x+(width-ui_text_width(font,value))/2,y+(height-font->height)/2,value,color,x+width,size);
    }
    void rounded(int x,int y,int width,int height,int radius,uint32_t color) {
        for(int py=0;py<height;++py)for(int px=0;px<width;++px) {
            if(x+px<0||x+px>=w||y+py<0||y+py>=h)continue;
            unsigned coverage=0;
            for(int sy=1;sy<4;sy+=2)for(int sx=1;sx<4;sx+=2){
                int ax=px*4+sx,ay=py*4+sy,dx=0,dy=0;
                if(ax<radius*4)dx=radius*4-ax;else if(ax>(width-radius)*4)dx=ax-(width-radius)*4;
                if(ay<radius*4)dy=radius*4-ay;else if(ay>(height-radius)*4)dy=ay-(height-radius)*4;
                if(dx*dx+dy*dy<=radius*radius*16)++coverage;
            }
            auto& pixel=pixels[(y+py)*w+x+px];
            if(coverage)pixel=ui_font_blend(pixel,color,(uint8_t)(coverage*255/4));
        }
    }
    // Small vector silhouettes, sampled at four points per pixel. Coordinates
    // use a 32-unit square so the same artwork fits sidebar and file rows.
    template<unsigned count>
    void polygon(int x,int y,int size,const Point (&points)[count],uint32_t color) {
        for(int py=0;py<size;++py)for(int px=0;px<size;++px){
            if(x+px<0||x+px>=w||y+py<0||y+py>=h)continue;
            unsigned coverage=0;
            for(int sy=1;sy<4;sy+=2)for(int sx=1;sx<4;sx+=2){
                int ax=(px*4+sx)*32,ay=(py*4+sy)*32;bool inside=false;
                for(unsigned i=0,j=count-1;i<count;j=i++){
                    int xi=points[i].x*size*4,yi=points[i].y*size*4;
                    int xj=points[j].x*size*4,yj=points[j].y*size*4;
                    if((yi>ay)!=(yj>ay)) {
                        int64_t cross=(int64_t)(xj-xi)*(ay-yi),edge=(int64_t)(ax-xi)*(yj-yi);
                        if((yj>yi&&edge<cross)||(yj<yi&&edge>cross))inside=!inside;
                    }
                }
                if(inside)++coverage;
            }
            auto& pixel=pixels[(y+py)*w+x+px];
            if(coverage)pixel=ui_font_blend(pixel,color,(uint8_t)(coverage*255/4));
        }
    }
    void line(int x0,int y0,int x1,int y1,uint32_t color) {
        int dx=x1>x0?x1-x0:x0-x1,dy=y1>y0?y1-y0:y0-y1;
        int sx=x0<x1?1:-1,sy=y0<y1?1:-1,error=dx-dy;
        for(;;){rect(x0,y0,1,1,color);if(x0==x1&&y0==y1)break;int e=error*2;
            if(e>-dy){error-=dy;x0+=sx;}if(e<dx){error+=dx;y0+=sy;}}
    }
    void folder(int x,int y,int size=28) {
        static const Point back[]={{2,7},{3,4},{12,4},{17,8},{29,8},{30,10},{29,26},{3,26}};
        static const Point tab[]={{4,6},{12,6},{16,10},{28,10},{28,24},{4,24}};
        static const Point face[]={{1,12},{31,12},{28,27},{3,27}};
        static const Point highlight[]={{2,12},{30,12},{29,14},{2,14}};
        static const Point shade[]={{4,25},{28,25},{28,27},{3,27}};
        polygon(x,y,size,back,0xffa77332);polygon(x,y,size,tab,0xffdfb362);
        polygon(x,y,size,face,0xffe9bd70);polygon(x,y,size,highlight,0xffffdc99);
        polygon(x,y,size,shade,0xffc79549);
    }
    void wizard(int x,int y,int size=28) {
        static const Point brim[]={{2,23},{7,20},{23,19},{30,23},{28,26},{19,28},{8,27}};
        static const Point crown[]={{7,21},{12,9},{18,3},{25,4},{20,8},{24,21},{17,24}};
        static const Point light[]={{12,10},{18,3},{22,4},{17,9},{13,21},{8,21}};
        static const Point band[]={{8,18},{22,17},{24,21},{17,24},{7,21}};
        static const Point star[]={{26,7},{27,10},{30,11},{27,12},{26,15},{25,12},{22,11},{25,10}};
        polygon(x,y,size,brim,0xff7b65bb);polygon(x,y,size,crown,0xff9a82db);
        polygon(x,y,size,light,0xffbba4f4);polygon(x,y,size,band,0xffedc985);
        polygon(x,y,size,star,0xffffdf9b);
    }
    void document(int x,int y,uint32_t accent=0xff8598b5) {
        rect(x+4,y+2,19,25,0xffd9e1ed);rect(x+17,y+2,6,6,0xff9eacc1);
        rect(x+7,y+12,12,2,accent);rect(x+7,y+17,12,2,accent);rect(x+7,y+22,8,2,accent);
    }
};
#endif
