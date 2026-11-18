#include <cassert>
#include <cstdio>
#define NATIVE_HOST_TEST
#include "user/task_manager.cpp"
static unsigned syncs;
static uint64_t native_call(uint64_t n,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t) {
    if(n==201)++syncs;
    return 0;
}
static uint32_t pixels[760*600];
static void process(int i,uint32_t pid,uint64_t cycles,const char* name,uint32_t kind=PROCESS_PROGRAM) {
    usage.incoming[i]={};usage.incoming[i].pid=pid;usage.incoming[i].cpu_cycles=cycles;
    native_copy(usage.incoming[i].name,256,name);
    usage.incoming_metadata.kinds[i]=kind;
}
int main(int argc,char** argv) {
    process(0,4,200,"/bin/busy.wiz");process(1,1,100,"Compositor",PROCESS_KERNEL);
    usage.incoming_metadata.tick_ms=1000;
    usage.incoming_totals={1000,100};usage.update(2);
    assert(!usage.sampled&&usage.cpu[0]==0);
    // Reordered records must match by PID, rather than row. A newcomer gets
    // a baseline, so its lifetime cycles cannot produce a false CPU spike.
    process(0,1,150,"Compositor",PROCESS_KERNEL);process(1,4,950,"/bin/busy.wiz");process(2,8,9999,"New app");
    usage.incoming_metadata.tick_ms=2000;
    usage.incoming_totals={2000,300};usage.update(3);
    assert(usage.sampled&&usage.total_cpu==800);
    assert(usage.cpu[0]==50&&usage.cpu[1]==750&&usage.cpu[2]==0);
    assert(usage.order[0]==1&&usage.order[1]==0);
    assert(usage.group_count[PROCESS_PROGRAM]==2&&usage.group_count[PROCESS_KERNEL]==1);
    assert(usage.row_count==6&&usage.rows[0]==-1&&usage.rows[1]==1&&usage.rows[2]==2&&usage.rows[4]==-3&&usage.rows[5]==0);
    assert(usage.history.count==1&&usage.history.at(0).cpu==800&&usage.history.at(0).tick==2000);
    surface={pixels,760,600};window_id=1;
    NativeEvent click{1,24,LIST_TOP+10,1,0,0};handle_event(click);
    assert(usage.collapsed[PROCESS_PROGRAM]&&usage.row_count==4&&usage.group_count[PROCESS_PROGRAM]==2);
    handle_event(click);assert(!usage.collapsed[PROCESS_PROGRAM]&&usage.row_count==6);
    CpuHistory history;
    for(unsigned i=0;i<80;++i)history.add(i*1000,i==79?800:(i%20)*45);
    assert(history.count==61&&history.at(0).tick==19000&&history.at(60).tick==79000);
    history.add(79000,500);assert(history.count==61&&history.at(60).cpu==800);
    history.add(80000,2000);assert(history.at(60).cpu==1000);
    usage.history=history;render();assert(syncs==1&&!dirty);
    assert(pixels[TABLE_TOP*760]==PANEL&&pixels[(TABLE_TOP+32)*760]==LINE);
    if(argc>1){FILE* f=fopen(argv[1],"wb");assert(f);fprintf(f,"P6\n760 600\n255\n");
        for(auto p:pixels){unsigned char rgb[]={(unsigned char)(p>>16),(unsigned char)(p>>8),(unsigned char)p};fwrite(rgb,1,3,f);}fclose(f);}
    usage.update(-1);assert(usage.failed&&usage.count==3&&usage.total_cpu==800);
    // Exit, then an idle interval: disappeared processes do not retain rows.
    process(0,1,150,"Compositor",PROCESS_KERNEL);usage.incoming_metadata.tick_ms=3000;
    usage.incoming_totals={3000,1300};usage.update(1);
    assert(!usage.failed&&usage.count==1&&usage.total_cpu==0&&usage.cpu[0]==0);
    usage.incoming_metadata.kinds[0]=99;usage.incoming_metadata.tick_ms=4000;
    usage.incoming_totals={4000,1300};usage.update(300);
    assert(usage.truncated&&usage.count==256);
    assert(usage.kinds[0]==PROCESS_BACKGROUND);
    char value[32];percent(1000,value);assert(native_length(value)==6);
    kernel_usage.incoming.sampled_at=10000;kernel_usage.incoming.tick=10;kernel_usage.update(0);
    assert(!kernel_usage.sampled);
    kernel_usage.incoming.sampled_at=20000;kernel_usage.incoming.tick=1010;
    kernel_usage.incoming.cycles[KCPU_INTERRUPTS]=1000;kernel_usage.incoming.calls[KCPU_INTERRUPTS]=1000;
    kernel_usage.incoming.cycles[KCPU_SCHEDULER]=2000;kernel_usage.incoming.calls[KCPU_SCHEDULER]=1100;
    kernel_usage.update(0);
    assert(kernel_usage.total==3000&&kernel_usage.cpu[KCPU_INTERRUPTS]==1000&&kernel_usage.calls[KCPU_INTERRUPTS]==1000);
    kernel_details=true;render();assert(syncs==2);
    surface={pixels,620,440};scroll=999;clamp_scroll();render();assert(scroll==row_count()-visible_rows());
    const int widths[]={620,760};
    for(int width:widths){
        surface.w=width;kernel_details=false;render();
        int x=details_x()+DETAILS_WIDTH/2;
        assert(pixels[(HEADER_Y+HEADER_ROW/2)*width+details_x()+8]==PANEL);
        handle_event({1,x,HEADER_Y+HEADER_ROW,1,0,0});assert(!kernel_details);
        handle_event({1,x,HEADER_Y+HEADER_ROW/2,1,0,0});assert(kernel_details&&scroll==0);
        handle_event({1,x,HEADER_Y+HEADER_ROW/2,1,0,0});assert(!kernel_details);
    }
    NativeEvent wheel{4,0,0,0,0,100};handle_event(wheel);assert(scroll==0);
    assert(handle_event({7,0,0,0,0,0}));
    kernel_usage.update(-1);assert(kernel_usage.failed&&kernel_usage.total==3000);
    kernel_usage.reset();assert(!kernel_usage.sampled&&!kernel_usage.initialized);
    kernel_usage.incoming={};kernel_usage.incoming.sampled_at=30000;kernel_usage.update(0);
    assert(!kernel_usage.sampled&&kernel_usage.total==0);
    puts("Task Manager sampling and rendering passed");
}
