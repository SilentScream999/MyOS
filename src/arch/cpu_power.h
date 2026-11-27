#ifndef WYZAR_CPU_POWER_H
#define WYZAR_CPU_POWER_H
#include "cpu_power_policy.h"
#include "msr_probe.h"
static cpu_power::Result g_cpu_power_boot;
static void init_cpu_power(){
    uint32_t a,b,c,d;
    auto cpuid=[&](uint32_t leaf){__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(leaf),"c"(0));};
    cpuid(0);uint32_t maximum=a;
    cpu_power::Features f{};
    f.intel=b==0x756e6547&&d==0x49656e69&&c==0x6c65746e;
    if(maximum>=1){cpuid(1);f.signature=a;f.hypervisor=c>>31;f.speedstep=c&(1u<<7);}
    if(maximum>=6){cpuid(6);f.thermal_features=a;}
    struct IO {
        bool read(uint32_t msr,uint64_t& value){return power_read_msr(msr,&value);}
        bool write(uint32_t msr,uint64_t value){return power_write_msr(msr,value);}
    } io;
    g_cpu_power_boot=cpu_power::initialize(f,io);
    using Status=cpu_power::Status;
    if(g_cpu_power_boot.status==Status::Applied){
        char number[24];
        print((char*)"[power] Restored minimum boot CPU request to advertised base ratio.");
        print((char*)"[power] PERF_CTL before:");to_hex(g_cpu_power_boot.before,number);print(number);
        print((char*)"[power] PERF_CTL after:");to_hex(g_cpu_power_boot.after,number);print(number);
    }else if(g_cpu_power_boot.status==Status::WriteFailed||g_cpu_power_boot.status==Status::VerifyFailed){
        print((char*)"[power] Boot CPU request repair failed; no retry or power-limit changes.");
    }
}
#endif
