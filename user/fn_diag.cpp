#include <stdint.h>
#include "src/drivers/fn_trace.h"
// This helper intentionally uses only the existing file,
// sleep/document syscalls and the bounded firmware diagnostic interface.
static int64_t call(uint64_t n,uint64_t a=0,uint64_t b=0,uint64_t c=0,uint64_t d=0,uint64_t e=0){
    register uint64_t r10 __asm__("r10")=d,r8 __asm__("r8")=e;int64_t result;
    __asm__ volatile("syscall":"=a"(result):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8):"rcx","r11","memory");return result;
}
static void text(const char* s){unsigned n=0;while(s[n])++n;call(1,1,(uint64_t)s,n);}
static uint8_t chunk[4096];
static bool write_all(int64_t fd,const void* data,unsigned length){
    const uint8_t* p=(const uint8_t*)data;
    while(length){int64_t n=call(1,fd,(uint64_t)p,length);if(n<=0||(uint64_t)n>length)return false;p+=n;length-=n;}
    return true;
}
struct Header {char magic[8];uint32_t trace_bytes,tables;};
static int run(){
    char path[256];if(call(215,(uint64_t)path,sizeof(path))<=0)return 1;
    call(83,(uint64_t)"/home",0755); // RAM recovery may not have this directory yet.
    int64_t started=call(225,0,40000);
    if(started<0){text("Cannot start capture (another capture may still be active).\n");return 1;}
    text("Fn-key capture: follow each prompt once. Ctrl+C cancels.\n");
    const char* stages[]={"Press Fn+F1 once.\n","Press Fn+F2 once.\n","Press Fn+F3 once.\n",
        "Press Fn+Esc once.\n","Press Fn+F1 once again.\n","Press Fn+F2 once again.\n","Press Fn+F3 once again.\n",
        "Press plain F1 once.\n","Press plain F2 once.\n","Press plain F3 once.\n"};
    for(unsigned i=0;i<10;++i){call(225,3,i);text(stages[i]);call(35,3000);}
    call(225,4);
    int64_t size=call(225,1);
    if(size<16||size>sizeof(FnTrace)){text("Capture read failed.\n");return 1;}
    int64_t fd=call(2,(uint64_t)path,0x241); // O_WRONLY|O_CREAT|O_TRUNC
    if(fd<0){text("Cannot save capture file.\n");return 1;}
    Header header={{'W','F','N','K','E','Y','1',0},(uint32_t)size,0};
    bool ok=write_all(fd,&header,sizeof(header));
    for(unsigned at=0;ok&&at<(unsigned)size;){
        unsigned n=size-at;if(n>sizeof(chunk))n=sizeof(chunk);
        int64_t got=call(225,1,at,(uint64_t)chunk,n);
        if(got!=(int64_t)n||!write_all(fd,chunk,n)){ok=false;break;}at+=n;
    }
    for(unsigned index=0;ok&&index<35;++index){
        int64_t bytes=call(225,2,index);
        if(bytes<0)continue;
        uint32_t record[2]={index,(uint32_t)bytes};ok=write_all(fd,record,sizeof(record));
        for(unsigned at=0;ok&&at<(unsigned)bytes;){
            unsigned n=bytes-at;if(n>sizeof(chunk))n=sizeof(chunk);
            int64_t got=call(225,2,index,(uint64_t)chunk,n,at);
            if(got!=(int64_t)n||!write_all(fd,chunk,n)){ok=false;break;}at+=n;
        }
        if(ok)++header.tables;
    }
    if(ok)ok=call(8,fd,0,0)==0&&write_all(fd,&header,sizeof(header));
    if(ok)ok=call(74,fd)==0; // FSYNC
    call(3,fd);
    text(ok?"Saved Fn-key capture: ":"Capture save failed: ");text(path);text("\n");
    text("Fn+Esc may have changed FnLock; press it again if desired.\n");
    return ok?0:1;
}
extern "C" __attribute__((noreturn)) void _start(){int result=run();call(60,result);for(;;){}}
