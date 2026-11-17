#include "src/core/process_info.h"
static uint64_t call(uint64_t nr,uint64_t a=0,uint64_t b=0,uint64_t c=0,uint64_t d=0) {
    register uint64_t r10 __asm__("r10")=d;uint64_t result;
    __asm__ volatile("syscall":"=a"(result):"a"(nr),"D"(a),"S"(b),"d"(c),"r"(r10):"rcx","r11","memory");return result;
}
static bool equal(const char* a,const char* b){while(*a&&*a==*b){++a;++b;}return *a==*b;}
static ProcessInfo rows[256];static ProcessMetadata metadata;static ProcessTotals totals;
static bool check() {
    uint64_t self=call(39);
    int64_t count=(int64_t)call(218,(uint64_t)rows,256,(uint64_t)&totals,(uint64_t)&metadata);
    if(count<=0||count>256||!metadata.tick_ms)return false;
    bool kernel=false,shell=false,program=false,manager=false;
    for(int i=0;i<count;++i) {
        if(metadata.kinds[i]>=PROCESS_KIND_COUNT)return false;
        if(rows[i].pid==0)kernel=metadata.kinds[i]==PROCESS_KERNEL;
        if(rows[i].pid==self)program=metadata.kinds[i]==PROCESS_PROGRAM;
        if(equal(rows[i].name,"/bin/init.elf"))shell=metadata.kinds[i]==PROCESS_BACKGROUND;
        if(equal(rows[i].name,"/bin/task_manager.wiz"))manager=metadata.kinds[i]==PROCESS_PROGRAM;
    }
    if(!kernel||!shell||!program||!manager)return false;
    if((int64_t)call(218,(uint64_t)rows,256,(uint64_t)&totals)<1)return false; // ABI 8 caller.
    if((int64_t)call(218,(uint64_t)rows,257,(uint64_t)&totals,(uint64_t)&metadata)!=-1)return false;
    if((int64_t)call(218,(uint64_t)rows,256,(uint64_t)&totals,0xffff800000000000ULL)!=-1)return false;
    if((int64_t)call(218,0,0,(uint64_t)&totals,(uint64_t)&metadata)<1)return false;
    uint64_t before=metadata.tick_ms;call(35,30);
    if((int64_t)call(218,(uint64_t)rows,1,(uint64_t)&totals,(uint64_t)&metadata)<count)return false;
    return metadata.tick_ms>before;
}
extern "C" void _start() {
    bool ok=check();const char* text=ok?"[process metadata] passed\n":"[process metadata] FAILED\n";
    unsigned n=0;while(text[n])++n;call(1,1,(uint64_t)text,n);call(60,ok?0:1);for(;;){}
}
