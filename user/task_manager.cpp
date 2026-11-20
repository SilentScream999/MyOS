#include "user/native_ui.h"
#include "src/core/process_info.h"
#include "user/task_usage.h"

static NativeSurface surface;
static uint64_t window_id;
static TaskUsage usage;
static KernelUsage kernel_usage;
static bool kernel_details=false;
static int scroll=0;
static bool dirty=true;
constexpr uint32_t BG=0xff1a1925,PANEL=0xff2b273b,INK=0xffeeebf8,MUTED=0xffa49bb6,LINE=0xff39334b;
constexpr int TABLE_TOP=220,LIST_TOP=254,ROW_HEIGHT=30;
constexpr int INSET=20,HEADER_Y=12,HEADER_ROW=32,DETAILS_WIDTH=152;
static int details_x(){return surface.w-INSET-DETAILS_WIDTH;}

static void percent(uint32_t tenths,char* out) {
    native_number(tenths/10,out);unsigned n=native_length(out);
    out[n++]='.';out[n++]=(char)('0'+tenths%10);out[n++]='%';out[n]=0;
}
static int visible_rows(){int rows=(surface.h-30-LIST_TOP)/ROW_HEIGHT;return rows>0?rows:1;}
static void kernel_percent(uint32_t hundredths,char* out) {
    native_number(hundredths/100,out);unsigned n=native_length(out);
    out[n++]='.';out[n++]=(char)('0'+hundredths/10%10);out[n++]=(char)('0'+hundredths%10);out[n++]='%';out[n]=0;
}
static int row_count(){return kernel_details?KCPU_COUNT+1:usage.row_count;}
static void clamp_scroll(){int max=row_count()-visible_rows();if(max<0)max=0;if(scroll<0)scroll=0;if(scroll>max)scroll=max;}
static void refresh() {
    int64_t count=(int64_t)native_call(218,(uint64_t)usage.incoming,TaskUsage::capacity,(uint64_t)&usage.incoming_totals,(uint64_t)&usage.incoming_metadata);
    usage.update(count);dirty=true;
    if(kernel_details)kernel_usage.update((int64_t)native_call(219,(uint64_t)&kernel_usage.incoming));
    clamp_scroll();
}
static void toggle_details(){
    kernel_details=!kernel_details;kernel_usage.reset();scroll=0;dirty=true;
    if(kernel_details)kernel_usage.update((int64_t)native_call(219,(uint64_t)&kernel_usage.incoming));
}
static void render_graph() {
    constexpr int panel_top=84,top=120,bottom=178,left=INSET+12;
    constexpr int title_mid=(panel_top+top)/2;
    int width=surface.w-104,height=bottom-top;
    surface.rounded(INSET,panel_top,surface.w-2*INSET,124,6,0xff211e30);
    const UiFont* label_font=ui_font_for_pixels(14);
    const UiFont* core_font=ui_font_for_pixels(12);
    const UiFont* value_font=ui_font_for_pixels(20);
    surface.text(left,title_mid-label_font->height/2,"CPU usage",INK);
    int core_x=left+ui_text_width(label_font,"CPU usage")+12;
    surface.text(core_x,title_mid-core_font->height/2,"Core 0",MUTED,0,12);
    char value[32];percent(usage.total_cpu,value);
    surface.text(surface.w-left-ui_text_width(value_font,value),title_mid-value_font->height/2,value,INK,0,20);
    for(int row=0;row<=2;++row)surface.rect(left,top+row*height/2,width,1,LINE);
    for(int col=0;col<=6;++col)surface.rect(left+col*(width-1)/6,top,1,height+1,LINE);
    surface.text(left+width+8,top-5,"100%",MUTED,0,12);
    surface.text(left+width+8,bottom-6,"0%",MUTED,0,12);
    surface.text(left,187,"60 seconds ago",MUTED,0,12);surface.text(left+width-24,187,"Now",MUTED,0,12);
    const auto& history=usage.history;if(!history.count)return;
    uint64_t now=history.at(history.count-1).tick;
    int previous_x=0,previous_y=0;bool have_previous=false;
    for(unsigned i=0;i<history.count;++i) {
        const auto& sample=history.at(i);uint64_t age=now-sample.tick;
        if(age>60000)continue;
        int x=left+width-1-(int)(age*(width-1)/60000);
        int y=bottom-(int)((uint64_t)sample.cpu*height/1000);
        if(have_previous) {
            for(int px=previous_x;px<=x;++px) {
                int py=x==previous_x?y:previous_y+(y-previous_y)*(px-previous_x)/(x-previous_x);
                surface.rect(px,py,1,bottom-py,0xff393052);
            }
            surface.line(previous_x,previous_y,x,y,0xffc6a6f4);
        }
        surface.rect(x-1,y-1,3,3,0xffc6a6f4);
        previous_x=x;previous_y=y;have_previous=true;
    }
}
static void render_programs() {
    int cpu=surface.w-110,state=surface.w-242,pid=surface.w-320;
    surface.rect(0,TABLE_TOP,surface.w,32,PANEL);surface.rect(0,TABLE_TOP+32,surface.w,1,LINE);
    surface.text(20,TABLE_TOP+8,"Process",MUTED,pid-12,12);surface.text(pid,TABLE_TOP+8,"PID",MUTED,state-12,12);
    surface.text(state,TABLE_TOP+8,"State",MUTED,cpu-12,12);surface.text(cpu,TABLE_TOP+8,"CPU %",MUTED,0,12);
    static const char* groups[]={"Programs","Background processes","Kernel / System"};
    for(int row=0;row<visible_rows()&&scroll+row<usage.row_count;++row) {
        int i=usage.rows[scroll+row],y=LIST_TOP+row*ROW_HEIGHT;char value[32];
        if(i<0) {
            unsigned group=(unsigned)(-1-i);
            surface.rect(0,y,surface.w,ROW_HEIGHT,0xff292438);
            if(usage.collapsed[group]){surface.line(22,y+11,27,y+15,MUTED);surface.line(27,y+15,22,y+19,MUTED);}
            else {surface.line(20,y+13,24,y+17,MUTED);surface.line(24,y+17,28,y+13,MUTED);}
            surface.text(38,y+7,groups[group],INK,pid-50);
            native_number(usage.group_count[group],value);surface.text(pid-30,y+8,value,MUTED,pid-4,12);
            continue;
        }
        const auto& p=usage.previous[i];
        if(row%2)surface.rect(0,y,surface.w,ROW_HEIGHT,0xff201d2d);
        const char* name=p.name;for(const char* s=p.name;*s;++s)if(*s=='/')name=s+1;
        if(p.name[0]!='/')name=p.name;
        surface.text(38,y+7,name,INK,pid-12);native_number(p.pid,value);surface.text(pid,y+7,value,MUTED,state-12,12);
        const char* states[]={"Ready","Running","Waiting","Exited"};
        surface.text(state,y+7,p.state<4?states[p.state]:"Unknown",MUTED,cpu-12,12);
        percent(usage.cpu[i],value);surface.text(cpu,y+7,value,usage.cpu[i]>=100?0xffe9bd70:INK,0,12);
    }
}
static void render_kernel() {
    static const char* labels[]={"Other kernel work","USB events / HID","USB hub maintenance","USB root ports",
        "Keyboard repeat","Interrupt handlers","Scheduler"};
    int cpu=surface.w-210,calls=surface.w-110;
    surface.rect(0,TABLE_TOP,surface.w,32,PANEL);surface.rect(0,TABLE_TOP+32,surface.w,1,LINE);
    surface.text(20,TABLE_TOP+8,"Kernel / Input activity",MUTED,cpu-12,12);
    surface.text(cpu,TABLE_TOP+8,"CPU %",MUTED,calls-12,12);surface.text(calls,TABLE_TOP+8,"Calls / sec",MUTED,0,12);
    for(int row=0;row<visible_rows()&&scroll+row<row_count();++row) {
        int i=scroll+row,y=LIST_TOP+row*ROW_HEIGHT;char value[32];
        if(row%2)surface.rect(0,y,surface.w,30,0xff201d2d);
        surface.text(20,y+7,i==KCPU_COUNT?"Kernel / Input total":labels[i],INK,cpu-12);
        kernel_percent(i==KCPU_COUNT?kernel_usage.total:kernel_usage.cpu[i],value);surface.text(cpu,y+7,value,INK,calls-12,12);
        if(i==KCPU_OTHER)surface.text(calls,y+7,"-",MUTED,0,12);
        else if(i<KCPU_COUNT){native_number(kernel_usage.calls[i],value);surface.text(calls,y+7,value,MUTED,0,12);}
    }
}
static void render() {
    surface.rect(0,0,surface.w,surface.h,BG);
    int title_y=HEADER_Y+(HEADER_ROW-ui_font_for_pixels(20)->height)/2;
    surface.text(INSET,title_y,"Task Manager",INK,0,20);
    surface.text(INSET,51,kernel_details?"Live breakdown of Kernel / Input CPU time":"Live processes - sorted by CPU within each section",MUTED,surface.w-INSET,12);
    surface.rounded(details_x(),HEADER_Y,DETAILS_WIDTH,HEADER_ROW,5,PANEL);
    surface.centered_text(details_x(),HEADER_Y,DETAILS_WIDTH,HEADER_ROW,kernel_details?"Processes (P)":"Kernel details (K)",INK,12);
    render_graph();if(kernel_details)render_kernel();else render_programs();
    if(row_count()>visible_rows()) {
        int track=visible_rows()*ROW_HEIGHT,thumb=track*visible_rows()/row_count();if(thumb<18)thumb=18;
        surface.rect(surface.w-7,LIST_TOP,3,track,LINE);
        surface.rect(surface.w-7,LIST_TOP+(track-thumb)*scroll/(row_count()-visible_rows()),3,thumb,0xffa38bc4);
    }
    surface.rect(0,surface.h-30,surface.w,30,PANEL);
    surface.text(20,surface.h-23,kernel_details?
        (kernel_usage.failed?"Kernel profiling is unavailable.":!kernel_usage.sampled?"Sampling kernel activity...":
         "Only Kernel / Input is profiled. Sleeping excluded. Profiling adds overhead."):
        usage.failed?"Could not read process information.":
        usage.truncated?"Showing first 256 processes. Scroll to browse.":
        !usage.sampled?"Sampling CPU usage...":"Click a section to collapse. Scroll to browse. CPU is measured on Core 0.",MUTED,surface.w-16,12);
    dirty=(int64_t)native_call(222,window_id)<0;
}
static bool handle_event(const NativeEvent& e) {
    if(e.type==7)return true;
    if(e.type==8&&native_accept_resize(window_id,surface)){clamp_scroll();dirty=true;}
    if(e.type==4){scroll-=e.delta*3;clamp_scroll();dirty=true;}
    if((e.type==1&&e.key==1&&e.y>=HEADER_Y&&e.y<HEADER_Y+HEADER_ROW&&e.x>=details_x()&&e.x<details_x()+DETAILS_WIDTH) ||
       (e.type==5&&((!kernel_details&&(e.key=='k'||e.key=='K'))||(kernel_details&&(e.key=='p'||e.key=='P')))))toggle_details();
    else if(!kernel_details&&e.type==1&&e.key==1&&e.y>=LIST_TOP&&e.y<LIST_TOP+visible_rows()*ROW_HEIGHT) {
        int row=scroll+(e.y-LIST_TOP)/ROW_HEIGHT;
        if(row<usage.row_count&&usage.rows[row]<0){unsigned group=(unsigned)(-1-usage.rows[row]);
            usage.collapsed[group]=!usage.collapsed[group];usage.rebuild_rows();clamp_scroll();dirty=true;}
    }
    return false;
}
#ifndef NATIVE_HOST_TEST
extern "C" void _start() {
    window_id=native_call(200,(uint64_t)"Task Manager",140,100,760,600);
    if(!window_id){native_call(60,1);for(;;){}}
    surface={(uint32_t*)native_call(203,window_id),760,600};
    if(!surface.pixels){native_call(60,1);for(;;){}}
    native_enable_resize(window_id,620,440,surface);refresh();
    unsigned ticks=0;bool exiting=false;
    while(!exiting) {
        NativeEvent e{};unsigned n=0;
        while(n++<64&&native_call(205,window_id,(uint64_t)&e)) {
            if(handle_event(e))exiting=true;
        }
        clamp_scroll();
        if(++ticks>=20){refresh();ticks=0;}
        if(dirty)render();native_call(35,50);
    }
    native_call(60,0);for(;;){}
}
#endif
