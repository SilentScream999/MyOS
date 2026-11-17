// Native PNG/JPEG viewer. The codec is also used by desktop resources.
#include "native_ui.h"
#include "native_file_dialog.h"
#include "src/gui/image.h"

// There is no userspace heap syscall yet. A bounded, coalescing arena keeps
// decoder scratch and successive documents reusable without kernel allocations.
constexpr uint64_t VIEWER_HEAP = 48ull*1024*1024;
struct alignas(16) ViewerBlock { uint64_t size; ViewerBlock* next; bool free; };
alignas(16) static uint8_t viewer_heap[VIEWER_HEAP];
static ViewerBlock* heap_head;
void* image_allocate(uint64_t size) {
    if(!size||size>VIEWER_HEAP-sizeof(ViewerBlock))return nullptr;
    size=(size+15)&~15ull;
    if(!heap_head){heap_head=(ViewerBlock*)viewer_heap;*heap_head={VIEWER_HEAP-sizeof(ViewerBlock),nullptr,true};}
    for(auto* b=heap_head;b;b=b->next)if(b->free&&b->size>=size){
        if(b->size>=size+sizeof(ViewerBlock)+16){auto* n=(ViewerBlock*)((uint8_t*)(b+1)+size);
            *n={b->size-size-sizeof(ViewerBlock),b->next,true};b->next=n;b->size=size;}
        b->free=false;return b+1;
    }
    return nullptr;
}
void image_deallocate(void* pointer) {
    if(!pointer)return;((ViewerBlock*)pointer-1)->free=true;
    for(auto* b=heap_head;b&&b->next;){if(b->free&&b->next->free){b->size+=sizeof(ViewerBlock)+b->next->size;b->next=b->next->next;}else b=b->next;}
}
#ifndef NATIVE_HOST_TEST
extern "C" int memcmp(const void* a,const void* b,unsigned long n){auto* x=(const uint8_t*)a;auto* y=(const uint8_t*)b;for(unsigned long i=0;i<n;++i)if(x[i]!=y[i])return x[i]<y[i]?-1:1;return 0;}
extern "C" void* memmove(void* dst,const void* src,unsigned long n){auto* d=(uint8_t*)dst;auto* s=(const uint8_t*)src;if(d<s){for(unsigned long i=0;i<n;++i)d[i]=s[i];}else for(unsigned long i=n;i;--i)d[i-1]=s[i-1];return dst;}
#endif
static NativeSurface surface{};
static NativeFileDialog picker;
static Image picture{};
static uint64_t window_id;
static char path[256],message[192]="Open a PNG or JPEG image (Ctrl+O).";
static bool dirty=true,exiting=false,fit=true,dragging=false;
static int hovered_button=-1;
static const NativeToolbarButton viewer_buttons[]={
    {10,10,76,32,NativeIcon::Open,"Open...","Open image (Ctrl+O)"},
    {96,10,48,32,NativeIcon::Minus,nullptr,"Zoom out (-)"},
    {152,10,48,32,NativeIcon::Plus,nullptr,"Zoom in (+)"},
    {210,10,66,32,NativeIcon::Fit,"Fit","Fit image to window (F)"},
    {284,10,72,32,NativeIcon::ActualSize,"100%","Show actual pixels (1)"}
};
static int zoom=100,pan_x=0,pan_y=0,last_x,last_y;
constexpr uint64_t VIEWER_FILE_LIMIT=8ull*1024*1024, VIEWER_PIXELS=4ull*1024*1024;
static void note(const char* text){native_copy(message,sizeof(message),text);dirty=true;}
static bool load(const char* candidate){
    file_info info{};
    if(candidate[0]!='/'||native_length(candidate)>=sizeof(path)||sys_stat(candidate,&info)!=0||info.type!=1){note("Cannot open that image file.");return false;}
    if(!info.size||info.size>VIEWER_FILE_LIMIT){note("Image file must be between 1 byte and 8 MiB.");return false;}
    auto* bytes=(uint8_t*)image_allocate(info.size);if(!bytes){note("Not enough image memory. Current image was kept.");return false;}
    uint64_t fd=sys_open(candidate,0),done=0;bool ok=fd!=(uint64_t)-1;
    while(ok&&done<info.size){uint64_t n=sys_read(fd,(char*)bytes+done,info.size-done);if(!n||n>info.size-done)ok=false;else done+=n;}
    if(fd!=(uint64_t)-1)sys_close(fd);
    Image next{};if(ok)ok=image_decode(bytes,info.size,&next,VIEWER_PIXELS);image_deallocate(bytes);
    if(!ok){note("Read/decode failed: use PNG/JPEG up to 4 MP; memory may be full.");return false;}
    image_free(&picture);picture=next;native_copy(path,sizeof(path),candidate);
    fit=true;pan_x=pan_y=0;dragging=false;note("Image opened.");return true;
}
struct ViewRect {int x,y,w,h;};
static ViewRect view_rect(){
    int w=picture.width,h=picture.height,available_h=surface.h-80;
    if(!w||!h)return {};
    if(fit){int available_w=surface.w-24;available_h-=24;
        if((int64_t)available_w*h<(int64_t)available_h*w){h=(int64_t)h*available_w/w;w=available_w;}
        else {w=(int64_t)w*available_h/h;h=available_h;}
    }else {w=(int64_t)w*zoom/100;h=(int64_t)h*zoom/100;}
    if(w<1)w=1;if(h<1)h=1;
    int limit_x=w>surface.w?(w-surface.w)/2+24:0;
    int limit_y=h>surface.h-80?(h-(surface.h-80))/2+24:0;
    if(pan_x>limit_x)pan_x=limit_x;if(pan_x<-limit_x)pan_x=-limit_x;
    if(pan_y>limit_y)pan_y=limit_y;if(pan_y<-limit_y)pan_y=-limit_y;
    return {(surface.w-w)/2+pan_x,52+(surface.h-80-h)/2+pan_y,w,h};
}
static void zoom_by(int direction){
    if(!picture.pixels)return;
    if(fit){auto r=view_rect();zoom=(int64_t)r.w*100/picture.width;fit=false;}
    zoom=direction>0?zoom*5/4+1:zoom*4/5;
    if(zoom<10)zoom=10;if(zoom>800)zoom=800;dirty=true;
}
static void open_picker(){dragging=false;hovered_button=-1;dirty=true;if(!picker.show(false,path[0]?path:"/home/"))note("Could not open file browser.");}
static void render(){
    surface.rect(0,0,surface.w,surface.h,0xff171923);
    if(picture.pixels){auto r=view_rect();
        int left=r.x>0?r.x:0,top=r.y>52?r.y:52,right=r.x+r.w,bottom=r.y+r.h;
        if(right>surface.w)right=surface.w;if(bottom>surface.h-28)bottom=surface.h-28;
        // Sample directly into the visible viewport: zoom never allocates an
        // enlarged bitmap. Premultiplied alpha is shown over a checkerboard.
        for(int y=top;y<bottom;++y)for(int x=left;x<right;++x){
            unsigned sx=(uint64_t)(x-r.x)*picture.width/r.w,sy=(uint64_t)(y-r.y)*picture.height/r.h;
            uint32_t bg=(((x-r.x)/12+(y-r.y)/12)&1)?0xff6a6b73:0xff8a8b93;
            surface.pixels[y*surface.w+x]=image_over(bg,picture.pixels[sy*picture.width+sx]);
        }
    }else {
        int title_h=ui_font_for_pixels(24)->height,body_h=ui_font_for_pixels(14)->height,hint_h=ui_font_for_pixels(12)->height;
        int total=64+18+title_h+8+body_h+8+hint_h;
        int y=52+(surface.h-80-total)/2;
        surface.image_file((surface.w-64)/2,y,64);y+=64+18;
        surface.centered_text(0,y,surface.w,title_h,"Open an image",0xffeeebfa,24);y+=title_h+8;
        surface.centered_text(0,y,surface.w,body_h,"Choose a PNG or JPEG to get started.",0xffb3aec5,14);y+=body_h+8;
        surface.centered_text(0,y,surface.w,hint_h,"Use Open or press Ctrl+O",0xff908ba6,12);
    }
    surface.rect(0,0,surface.w,52,0xff242334);
    for(int i=0;i<5;++i)viewer_buttons[i].draw(surface,i==0||picture.pixels,hovered_button==i,
        (i==3&&fit)||(i==4&&!fit&&zoom==100));
    if(path[0]){const char* name=path;for(const char* p=path;*p;++p)if(*p=='/')name=p+1;
        surface.text(440,18,name,0xffb3aec5,surface.w-12,12);}
    if(picture.pixels){char n[24];auto r=view_rect();native_number((uint64_t)r.w*100/picture.width,n);
        surface.text(374,18,n,0xffd5c8ed,surface.w-40,12);surface.text(374+ui_text_width(ui_font_for_pixels(12),n),18,"%",0xffd5c8ed,surface.w-12,12);}
    surface.rect(0,surface.h-28,surface.w,28,0xff242334);
    surface.text(12,surface.h-21,hovered_button>=0?viewer_buttons[hovered_button].hint:message,
        0xffc6bfd8,surface.w-(picture.pixels?152:12),12);
    if(picture.pixels){char w[24],h[24];native_number(picture.width,w);native_number(picture.height,h);
        int x=surface.w-136;surface.text(x,surface.h-21,w,0xffc6bfd8,surface.w-12,12);
        x+=ui_text_width(ui_font_for_pixels(12),w);surface.text(x,surface.h-21," x ",0xffc6bfd8,surface.w-12,12);
        x+=ui_text_width(ui_font_for_pixels(12)," x ");surface.text(x,surface.h-21,h,0xffc6bfd8,surface.w-12,12);}
    native_call(201,window_id);dirty=false;
}
static void handle_event(const NativeEvent& e){
    if(e.type==7){exiting=true;return;}
    if(e.type==8){if(native_accept_resize(window_id,surface))dirty=true;return;}
    if(picker.window)return;
    if(e.type==5){
        if((e.mods&1)&&(e.key=='o'||e.key=='O'))open_picker();
        else if(e.key=='+'||e.key=='=')zoom_by(1);else if(e.key=='-')zoom_by(-1);
        else if(e.key=='0'||e.key=='f'||e.key=='F'){fit=true;pan_x=pan_y=0;dirty=true;}
        else if(e.key=='1'){fit=false;zoom=100;pan_x=pan_y=0;dirty=true;}
        else if(e.key>=0x1041&&e.key<=0x1044){if(fit)zoom=100;fit=false;
            if(e.key==0x1044)pan_x+=40;if(e.key==0x1043)pan_x-=40;
            if(e.key==0x1041)pan_y+=40;if(e.key==0x1042)pan_y-=40;dirty=true;}
        return;
    }
    if(e.type==4){zoom_by(e.delta>0?1:-1);return;}
    if(e.type==2){dragging=false;return;}
    if(e.type==3){
        if(dragging){pan_x+=e.x-last_x;pan_y+=e.y-last_y;last_x=e.x;last_y=e.y;dirty=true;return;}
        int next=-1;for(int i=0;i<5;++i)if((i==0||picture.pixels)&&viewer_buttons[i].hit(e.x,e.y))next=i;
        if(next!=hovered_button){hovered_button=next;dirty=true;}return;
    }
    if(e.type!=1||e.key!=1)return;
    if(e.y>=10&&e.y<42){for(int i=0;i<5;++i)if(viewer_buttons[i].hit(e.x,e.y)){
        if(i==0)open_picker();else if(!picture.pixels)return;
        else if(i==1)zoom_by(-1);else if(i==2)zoom_by(1);
        else {fit=i==3;if(!fit)zoom=100;pan_x=pan_y=0;dirty=true;}return;}
    }else if(e.y>=52&&e.y<surface.h-28&&picture.pixels&&!fit){dragging=true;last_x=e.x;last_y=e.y;}
}
#ifndef NATIVE_HOST_TEST
extern "C" void _start(){
    window_id=native_call(200,(uint64_t)"Image Viewer",160,100,800,560);
    if(!window_id){native_call(60,1);for(;;){}}
    surface={(uint32_t*)native_call(203,window_id),800,560};
    if(!surface.pixels){native_call(60,1);for(;;){}}
    native_enable_resize(window_id,480,300,surface);
    char document[256]{};if((int64_t)native_call(215,(uint64_t)document,sizeof(document))>0)load(document);
    unsigned frame=0;
    while(!exiting){NativeEvent e{};unsigned n=0;
        while(n++<64&&native_call(205,window_id,(uint64_t)&e))handle_event(e);
        n=0;while(n++<64&&picker.window&&native_call(205,picker.window,(uint64_t)&e)){
            auto result=picker.event(e,frame);if(result==NativeFileDialog::ACCEPT){if(load(picker.result))picker.hide();else picker.tell(message);}
            else if(result==NativeFileDialog::CANCEL)picker.hide();}
        if(dirty)render();if(picker.window){picker.tick(frame);if(picker.dirty)picker.render();}
        ++frame;native_call(35,16);
    }
    picker.hide();image_free(&picture);native_call(60,0);for(;;){}
}
#endif
