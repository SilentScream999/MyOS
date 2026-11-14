typedef unsigned long u64;
static u64 call(u64 nr,u64 a,u64 b,u64 c){u64 r;__asm__ volatile("syscall":"=a"(r):"a"(nr),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");return r;}
static void say(const char* text){u64 n=0;while(text[n])++n;call(1,1,(u64)text,n);}
void _start(void){
    say("[console-probe] input ready\n");char line[128];u64 used=0;
    while(used<127){u64 n=call(0,0,(u64)(line+used),1);if(n!=1)break;if(line[used++]=='\n')break;}
    line[used]=0;say("[console-probe] received: ");say(line);say("[console-probe] waiting for Ctrl+C\n");
    for(;;)call(35,100,0,0);
}
