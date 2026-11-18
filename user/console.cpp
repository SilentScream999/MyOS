#include "user/native_ui.h"
#include "src/gui/font8x8_basic.h"
#include "src/core/app.h"
// Match the shell: an unstretched 8x8 bitmap in an 8x10 character cell.
constexpr int CELL_W=8,CELL_H=10,MAX_COLS=480,MAX_ROWS=216,HISTORY_ROWS=500;
static int W=800,H=600,COLS=100,ROWS=60;
static char cells[MAX_ROWS][MAX_COLS];static int col=0,row=0,escape=0;
static uint32_t colors[MAX_ROWS][MAX_COLS];
static char history[HISTORY_ROWS][MAX_COLS];
static uint32_t history_colors[HISTORY_ROWS][MAX_COLS];
static int history_start=0,history_size=0,view_offset=0;
static uint32_t text_color=0xffffffff;
static char transcript[262144];static unsigned transcript_start=0,transcript_size=0;
static bool changed=true,running=true;static uint64_t exit_code=0;
static NativeSurface screen{};
static bool interactive=false;
static unsigned escape_value=0;
static uint64_t cursor_epoch=0,now=0;
static bool cursor_visible=true,cursor_drawn=false,active=true;
static void scroll_view(int delta) {
    view_offset+=delta;
    if(view_offset<0)view_offset=0;
    if(view_offset>history_size)view_offset=history_size;
    changed=true;
}
static void terminal_char(char ch) {
    if(escape){
        if(ch=='['){escape=2;escape_value=0;return;}
        if(ch>='0'&&ch<='9'){if(escape_value<10000)escape_value=escape_value*10+ch-'0';return;}
        if(ch>='@'&&ch<='~'){
            unsigned count=escape_value?escape_value:1;
            if(ch=='J'&&escape_value==2){
                for(auto& line:cells)for(char& c:line)c=0;
                history_start=history_size=view_offset=0;row=col=0;
            }
            if(ch=='H')row=col=0;
            if(ch=='m'){
                if(escape_value==0)text_color=0xffffffff;
                if(escape_value==32)text_color=0xff00ff00;
            }
            if(ch=='K')for(int x=escape_value==2?0:col;x<COLS;++x)cells[row][x]=0;
            if(ch=='D'||ch=='C'){
                unsigned position=row*COLS+col;
                if(ch=='D')position=count>position?0:position-count;
                else {
                    position+=count;
                    if(position>=(unsigned)(ROWS*COLS))position=ROWS*COLS-1;
                }
                row=position/COLS;col=position%COLS;
            }
            escape=0;changed=true;
        }
        return;
    }
    if(ch==27){escape=1;return;}
    if(ch=='\r')col=0;
    else if(ch=='\n'){col=0;++row;}
    else if(ch=='\b'){if(col){--col;cells[row][col]=0;}}
    else if(ch=='\t'){int n=4-col%4;while(n--)terminal_char(' ');return;}
    else if((unsigned char)ch>=32){cells[row][col]=ch;colors[row][col]=text_color;if(++col==COLS){col=0;++row;}}
    if(row==ROWS){
        int saved=(history_start+history_size)%HISTORY_ROWS;
        for(int x=0;x<COLS;++x){history[saved][x]=cells[0][x];history_colors[saved][x]=colors[0][x];}
        if(history_size<HISTORY_ROWS)++history_size;else history_start=(history_start+1)%HISTORY_ROWS;
        if(view_offset&&view_offset<history_size)++view_offset;
        for(int y=1;y<ROWS;++y)for(int x=0;x<COLS;++x){cells[y-1][x]=cells[y][x];colors[y-1][x]=colors[y][x];}
        for(int x=0;x<COLS;++x)cells[ROWS-1][x]=0;row=ROWS-1;}
    changed=true;
}
static void put(char ch) {
    if(transcript_size==sizeof(transcript)){transcript_start=(transcript_start+1)%sizeof(transcript);--transcript_size;}
    transcript[(transcript_start+transcript_size++)%sizeof(transcript)]=ch;terminal_char(ch);
    cursor_epoch=now;
}
static void layout_console(int width,int height) {
    W=width;H=height;COLS=W/CELL_W;ROWS=H/CELL_H;
    if(COLS>MAX_COLS)COLS=MAX_COLS;if(ROWS>MAX_ROWS)ROWS=MAX_ROWS;
    if(COLS<1)COLS=1;if(ROWS<1)ROWS=1;
    for(auto& line:cells)for(char& c:line)c=0;col=row=escape=0;
    history_start=history_size=view_offset=0;text_color=0xffffffff;escape_value=0;
    for(unsigned i=0;i<transcript_size;++i)terminal_char(transcript[(transcript_start+i)%sizeof(transcript)]);
    changed=true;
}
static void render(uint64_t win) {
    screen.rect(0,0,W,H,0xff0a0a14);
    for(int y=0;y<ROWS;++y)for(int x=0;x<COLS;++x){
        int source=history_size-view_offset+y;
        bool past=source<history_size;
        int saved=(history_start+source)%HISTORY_ROWS;
        int live=source-history_size;
        unsigned char c=past?history[saved][x]:cells[live][x];if(c<32||c>126)continue;
        uint32_t color=past?history_colors[saved][x]:colors[live][x];
        for(int gy=0;gy<8;++gy)for(int gx=0;gx<8;++gx)if(font8x8_basic[c][gy]&(1u<<gx))
            screen.rect(x*CELL_W+gx,y*CELL_H+gy,1,1,color);}
    cursor_drawn=running&&active&&!view_offset&&cursor_visible;
    if(cursor_drawn)screen.rect(col*CELL_W,row*CELL_H+8,8,2,text_color);
    native_call(201,win);changed=false;
}
#ifndef NATIVE_HOST_TEST
extern "C" void _start() {
    char target[256];if((int64_t)native_call(214,0,(uint64_t)target,sizeof(target))<=0){native_call(60,1);for(;;){}}
    const char* name=target;for(const char* p=target;*p;++p)if(*p=='/')name=p+1;
    interactive=app_path_is_shell(target);
    char title[64]="Console - ";unsigned at=10;
    for(unsigned i=0;name[i]&&at<63;++i)title[at++]=name[i];title[at]=0;
    if(interactive)native_copy(title,sizeof(title),"Terminal");
    int32_t geometry[]={260,170,W,H};
    native_call(214,6,(uint64_t)geometry,sizeof(geometry));
    W=geometry[2];H=geometry[3];
    uint64_t win=native_call(200,(uint64_t)title,geometry[0],geometry[1],W,H);if(!win){native_call(60,1);for(;;){}}
    screen={(uint32_t*)native_call(203,win),W,H};
    if(!screen.pixels){native_call(60,1);for(;;){}}
    native_enable_resize(win,360,220,screen);layout_console(W,H);
    int64_t child=native_call(214,5);
    if(!child)child=native_call(59,(uint64_t)target);
    if(child<=0){running=false;exit_code=1;const char* error="Could not start this application.\n";while(*error)put(*error++);}
    for(;;){
        NativeEvent event{};while(native_call(205,win,(uint64_t)&event)){
            if(event.type==7){native_call(60,0);for(;;){}}
            if(event.type==8){if(native_accept_resize(win,screen))layout_console(screen.w,screen.h);continue;}
            if(event.type==4){scroll_view(-event.delta);continue;}
            if(event.type==5&&(event.mods&2)&&(event.key==0x1041||event.key==0x1042)){
                scroll_view(event.key==0x1041?1:-1);continue;
            }
            if(event.type==5&&running){
                if(view_offset){view_offset=0;changed=true;}
                if((event.mods&3)==3&&(event.key=='c'||event.key=='C')){
                    char copy=3;native_call(214,2,(uint64_t)&copy,1);
                }
                else if((event.mods&1)&&(event.key=='c'||event.key=='C'||event.key==3))native_call(214,4);
                else if((event.mods&1)&&(event.key=='d'||event.key=='D'))native_call(214,3);
                else if((event.mods&1)&&(event.key=='v'||event.key=='V')){
                    char pasted[4096];int64_t count=native_clipboard_get(pasted,sizeof(pasted));
                    bool valid=count>0;
                    for(int64_t i=0;i<count;++i)if(pasted[i]<32||pasted[i]>126){valid=false;break;}
                    if(valid)native_call(214,2,(uint64_t)pasted,count);
                }
                else if(event.key>=0x1041&&event.key<=0x1044){
                    char sequence[]={27,'[',(char)(event.key-0x1000)};
                    native_call(214,2,(uint64_t)sequence,sizeof(sequence));
                }
                else if(event.key<128){char ch=(char)event.key;native_call(214,2,(uint64_t)&ch,1);}
            }
        }
        char output[2048];for(unsigned batch=0;batch<8;++batch){int64_t n=native_call(214,1,(uint64_t)output,sizeof(output));
            if(n<=0)break;for(int64_t i=0;i<n;++i)put(output[i]);}
        if(running&&(int64_t)native_call(212,child,(uint64_t)&exit_code)>0){running=false;changed=true;
            put('\n');const char* message="Process exited with code ";while(*message)put(*message++);
            char number[24];native_number(exit_code,number);for(const char* p=number;*p;++p)put(*p);put('.');put('\n');}
        cursor_visible=((now-cursor_epoch)/500%2)==0;
        bool draw_cursor=running&&active&&!view_offset&&cursor_visible;
        if(changed)render(win);
        else if(draw_cursor!=cursor_drawn){
            screen.rect(col*CELL_W,row*CELL_H+8,8,2,draw_cursor?text_color:0xff0a0a14);
            native_call(204,win,col*CELL_W,row*CELL_H+8,8,2);
            cursor_drawn=draw_cursor;
        }
        uint64_t state=native_call(214,7,win);
        now=state>>1;active=(state&1)!=0;
    }
}
#endif
