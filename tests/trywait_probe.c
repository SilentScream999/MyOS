typedef unsigned long u64;
static u64 call(u64 nr,u64 a,u64 b) {
    u64 r;__asm__ volatile("syscall":"=a"(r):"a"(nr),"D"(a),"S"(b):"rcx","r11","memory");return r;
}
static void output(const char* s) {
    u64 n=0;while(s[n])++n;
    __asm__ volatile("syscall"::"a"(1UL),"D"(1UL),"S"(s),"d"(n):"rcx","r11","memory");
}
void _start(void) {
    u64 status=12345,pid=call(59,(u64)"/bin/counter.elf",0);
    if((long)pid<=0||call(212,pid,(u64)&status)!=0||status!=12345)goto fail;
    call(35,3500,0);
    if(call(212,pid,(u64)&status)!=pid||status!=0||(long)call(212,pid,0)!=-1||
       (long)call(212,1,0)!=-1)goto fail;
    output("[trywait] passed running, completion and repeat checks\n");call(60,0,0);
fail:
    output("[trywait] FAILED\n");call(60,1,0);for(;;){}
}
