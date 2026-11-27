#include <cassert>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#include "src/drivers/power_diag.h"
#include "src/arch/msr_probe.h"
static volatile sig_atomic_t recovered;
static void fault_handler(int, siginfo_t*,void* context){
    auto* c=(ucontext_t*)context;uint64_t rip=c->uc_mcontext.gregs[REG_RIP];
    // RDMSR at ring 3 produces the same #GP(0). Supply the kernel CS tag here;
    // the separate unit checks below verify rejection of real user frames.
    if(!power_msr_fixup(c->uc_mcontext.gregs[REG_TRAPNO],c->uc_mcontext.gregs[REG_ERR],8,rip))_exit(2);
    c->uc_mcontext.gregs[REG_RIP]=rip;recovered=recovered+1;
}
int main(){
    using namespace power_diag;
    assert(client_model(0x806e9));assert(client_model(0x506e3));
    assert(!client_model(0x306a9));assert(!client_model(0x1080f10));
    for(unsigned i=0;i<Count;++i){
        assert(!allowed(i,false,true,~0u,~0u));
        assert(!allowed(i,true,false,0,0));
    }
    assert(allowed(Aperf,true,false,0,1));assert(!allowed(Aperf,true,true,~0u,0));
    assert(allowed(HwpEnable,true,false,1u<<7,0));
    assert(!allowed(HwpPackageRequest,true,true,1u<<7,0));
    assert(allowed(HwpPackageRequest,true,false,(1u<<7)|(1u<<11),0));
    assert(allowed(PackageLimit,true,true,0,0));assert(!allowed(PackageLimit,true,false,~0u,~0u));
    // Recovery must never hide another kernel exception or any user exception.
    uint64_t fault=(uint64_t)power_msr_fault,rip=fault;
    assert(!power_msr_fixup(14,0,8,rip)&&rip==fault);
    assert(!power_msr_fixup(13,1,8,rip)&&rip==fault);
    assert(!power_msr_fixup(13,0,0x23,rip)&&rip==fault);
    ++rip;assert(!power_msr_fixup(13,0,8,rip)&&rip==fault+1);
    rip=fault;assert(power_msr_fixup(13,0,8,rip)&&rip==(uint64_t)power_msr_failed);
    assert(gen9_mhz(0)==0);assert(gen9_mhz(18u<<23)==300);assert(gen9_mhz(63u<<23)==1050);
    assert(gen9_mhz((63u<<23)|0x7fffff)==1050);
    // Execute the real assembly error path on Linux. Replace only CLI in this
    // test process (ring 3 cannot execute it); the original RDMSR must fault.
    auto* code=(unsigned char*)(void*)power_read_msr;
    long pagesize=sysconf(_SC_PAGESIZE);
    assert(mprotect((void*)((uintptr_t)code&~(pagesize-1)),pagesize,PROT_READ|PROT_WRITE|PROT_EXEC)==0);
    assert(code[0]==0x9c&&code[1]==0xfa);code[1]=0x90;
    struct sigaction action{};action.sa_sigaction=fault_handler;action.sa_flags=SA_SIGINFO;
    assert(sigaction(SIGSEGV,&action,nullptr)==0);
    uint64_t value=0xabcdef;
    assert(!power_read_msr(0xffffffff,&value)&&value==0xabcdef&&recovered==1);
    assert(!power_read_msr(0xe8,&value)&&value==0xabcdef&&recovered==2);
    uint64_t write_fault=(uint64_t)power_msr_write_fault; rip=write_fault;
    assert(!power_msr_fixup(13,0,0x23,rip)&&rip==write_fault);
    assert(power_msr_fixup(13,0,8,rip)&&rip==(uint64_t)power_msr_write_failed);
    code=(unsigned char*)(void*)power_write_msr;
    assert(mprotect((void*)((uintptr_t)code&~(pagesize-1)),pagesize,PROT_READ|PROT_WRITE|PROT_EXEC)==0);
    assert(code[0]==0x9c&&code[1]==0xfa);code[1]=0x90;
    assert(!power_write_msr(0x199,0x1d00)&&recovered==3);
    puts("PASS: CPU/model gates, GPU MHz decoding and actual RDMSR/WRMSR fault/stack recovery");
}
