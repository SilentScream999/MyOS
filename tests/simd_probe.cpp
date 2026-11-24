// Keep every XMM register live across syscalls and timer-driven preemption.
#include <stdint.h>
struct alignas(16) FxState { uint8_t bytes[512]; };
static FxState expected,observed;
static uint64_t call(uint64_t nr,uint64_t a=0,uint64_t b=0,uint64_t c=0,uint64_t d=0,uint64_t e=0){uint64_t result;
    register uint64_t r10 __asm__("r10")=d,r8 __asm__("r8")=e;
    __asm__ volatile("syscall":"=a"(result):"a"(nr),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8):"rcx","r11","memory");return result;}
static bool same(){
    // Defined control/status fields and the entire XMM0-15 register bank.
    for(unsigned i=0;i<5;++i)if(expected.bytes[i]!=observed.bytes[i])return false;
    for(unsigned r=0;r<8;++r)for(unsigned i=0;i<10;++i)
        if(expected.bytes[32+r*16+i]!=observed.bytes[32+r*16+i])return false;
    for(unsigned i=24;i<28;++i)if(expected.bytes[i]!=observed.bytes[i])return false;
    for(unsigned i=160;i<416;++i)if(expected.bytes[i]!=observed.bytes[i])return false;
    return true;
}
#define XMM_CLOBBERS "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7","xmm8","xmm9","xmm10","xmm11","xmm12","xmm13","xmm14","xmm15"
static bool syscalls(){
    __asm__ volatile(
        "fxrstor64 (%0)\n"
        "mov $64, %%r12\n"
        "1: mov $35, %%rax\n mov $1, %%rdi\n syscall\n dec %%r12\n jnz 1b\n"
        "fxsave64 (%1)\n"
        ::"r"(&expected),"r"(&observed):"rax","rdi","rcx","r11","r12",XMM_CLOBBERS,"memory","cc");
    return same();
}
static bool preemption(){
    __asm__ volatile(
        "fxrstor64 (%0)\n mov $10000000, %%rcx\n"
        "1: pause\n dec %%rcx\n jnz 1b\n"
        "fxsave64 (%1)\n"
        ::"r"(&expected),"r"(&observed):"rcx",XMM_CLOBBERS,"memory","cc");
    return same();
}
extern "C" void _start(){
    // Leave real values on the x87 stack as well as patterns in every XMM.
    __asm__ volatile("fninit; fld1; fldpi; fxsave64 (%0); fninit"::"r"(&expected):"memory");
    expected.bytes[24]=0x80;expected.bytes[25]=0x5f; // nondefault SSE rounding
    for(unsigned i=160;i<416;++i)expected.bytes[i]=(uint8_t)(i*37+11);
    bool syscall_ok=syscalls(),irq_ok=preemption();
    const char* result=syscall_ok?(irq_ok?"SIMD PASS\n":"SIMD FAIL IRQ\n"):(irq_ok?"SIMD FAIL SYSCALL\n":"SIMD FAIL BOTH\n");
    unsigned n=0;while(result[n])++n;call(1,1,(uint64_t)result,n);
    // Report independently of the terminal frontend's private text storage.
    // The result is computed above, before these reporting-only syscalls.
    call(200,(uint64_t)result,160,160,320,100);
    call(35,2000);
    call(60,!(syscall_ok&&irq_ok));for(;;){}
}
