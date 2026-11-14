#include "user/native_ui.h"
#include "src/gui/font8x8_basic.h"
// Match the shell: an unstretched 8x8 bitmap in an 8x10 character cell.
constexpr int COLS=88,CELL_W=8,CELL_H=10,W=744,H=440,ROWS=(H-28-40)/CELL_H;
static char cells[ROWS][COLS];static int col=0,row=0,escape=0;
static bool changed=true,running=true;static uint64_t exit_code=0;
static NativeSurface screen{};
static void put(char ch) {
    if(escape){if(ch=='['){escape=2;return;}if(ch>='@'&&ch<='~'){if(ch=='J'){for(auto& line:cells)for(char& c:line)c=0;row=col=0;}escape=0;}return;}
    if(ch==27){escape=1;return;}
    if(ch=='\r')col=0;
    else if(ch=='\n'){col=0;++row;}
    else if(ch=='\b'){if(col)--col;}
    else if(ch=='\t'){int n=4-col%4;while(n--)put(' ');return;}
    else if((unsigned char)ch>=32){cells[row][col]=ch;if(++col==COLS){col=0;++row;}}
    if(row==ROWS){for(int y=1;y<ROWS;++y)for(int x=0;x<COLS;++x)cells[y-1][x]=cells[y][x];
        for(int x=0;x<COLS;++x)cells[ROWS-1][x]=0;row=ROWS-1;}
    changed=true;
}
static void render(uint64_t win) {
    screen.rect(0,0,W,H,0xff0a0a14);screen.rect(0,0,W,32,0xff282137);
    screen.text(12,8,running?"Console application":"Application finished",0xffd6c2fa,W-110,12);
    screen.rounded(W-100,4,90,24,5,0xff43314f);
    screen.centered_text(W-100,4,90,24,running?"Interrupt":"Close",0xffeee6fb,12);
    for(int y=0;y<ROWS;++y)for(int x=0;x<COLS;++x){unsigned char c=cells[y][x];if(c<32||c>126)continue;
        for(int gy=0;gy<8;++gy)for(int gx=0;gx<8;++gx)if(font8x8_basic[c][gy]&(1u<<gx))
            screen.rect(12+x*CELL_W+gx,40+y*CELL_H+gy,1,1,0xffffffff);}
    screen.rect(0,H-28,W,28,0xff282137);
    if(running)screen.rect(12+col*CELL_W,40+row*CELL_H+8,7,1,0xffbba4f4);
    char status[96]="Type here. Ctrl+C interrupts only this application.";
    if(!running){char n[24];native_number(exit_code,n);native_copy(status,sizeof(status),"Exit code: ");unsigned at=native_length(status);
        for(unsigned i=0;n[i]&&at+1<sizeof(status);++i)status[at++]=n[i];status[at]=0;}
    screen.text(12,H-21,status,0xffb6a7c9,W-12,12);
    native_call(201,win);changed=false;
}
#ifndef NATIVE_HOST_TEST
extern "C" void _start() {
    char target[256];if((int64_t)native_call(214,0,(uint64_t)target,sizeof(target))<=0){native_call(60,1);for(;;){}}
    const char* name=target;for(const char* p=target;*p;++p)if(*p=='/')name=p+1;
    char title[64]="Console - ";unsigned at=10;
    for(unsigned i=0;name[i]&&at<63;++i)title[at++]=name[i];title[at]=0;
    uint64_t win=native_call(200,(uint64_t)title,260,170,W,H);if(!win){native_call(60,1);for(;;){}}
    screen={(uint32_t*)native_call(203,win),W,H};
    if(!screen.pixels){native_call(60,1);for(;;){}}
    int64_t child=native_call(59,(uint64_t)target);
    if(child<=0){running=false;exit_code=1;const char* error="Could not start this application.\n";while(*error)put(*error++);}
    for(;;){
        NativeEvent event{};while(native_call(205,win,(uint64_t)&event)){
            if(event.type==7){native_call(60,0);for(;;){}}
            if(event.type==1&&event.key==1&&event.x>=W-100&&event.y<32){if(running)native_call(214,4);else{native_call(60,0);for(;;){}}}
            if(event.type==5&&running){
                if((event.mods&1)&&(event.key=='c'||event.key=='C'||event.key==3))native_call(214,4);
                else if((event.mods&1)&&(event.key=='d'||event.key=='D'))native_call(214,3);
                else if(event.key<128){char ch=(char)event.key;native_call(214,2,(uint64_t)&ch,1);}
            }
        }
        char output[2048];for(unsigned batch=0;batch<8;++batch){int64_t n=native_call(214,1,(uint64_t)output,sizeof(output));
            if(n<=0)break;for(int64_t i=0;i<n;++i)put(output[i]);}
        if(running&&(int64_t)native_call(212,child,(uint64_t)&exit_code)>0){running=false;changed=true;
            put('\n');const char* message="Process exited with code ";while(*message)put(*message++);
            char number[24];native_number(exit_code,number);for(const char* p=number;*p;++p)put(*p);put('.');put('\n');}
        if(changed)render(win);native_call(35,16);
    }
}
#endif
