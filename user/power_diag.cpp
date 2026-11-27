#include <stdint.h>
#include "src/drivers/power_diag.h"
static int64_t call(uint64_t n,uint64_t a=0,uint64_t b=0,uint64_t c=0,uint64_t d=0){
    register uint64_t r10 __asm__("r10")=d,r8 __asm__("r8")=0;int64_t result;
    __asm__ volatile("syscall":"=a"(result):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8):"rcx","r11","memory");return result;
}
static unsigned length(const char* s){unsigned n=0;while(s[n])++n;return n;}
static void print(const char* s){call(1,1,(uint64_t)s,length(s));}
static char report[16384];static unsigned used;
static void put(const char* s){while(*s&&used+1<sizeof(report))report[used++]=*s++;report[used]=0;}
static void number(uint64_t v,unsigned base=10){char b[32];unsigned n=0;do{b[n++]="0123456789abcdef"[v%base];v/=base;}while(v);while(n){char t[2]={b[--n],0};put(t);}}
static void field(const char* name,uint64_t v){put(name);put("=");number(v);put("\n");}
static void hexfield(const char* name,uint64_t v){put(name);put("=0x");number(v,16);put("\n");}
static bool sample(power_diag::Sample& s){return call(225,5,0,(uint64_t)&s,sizeof(s))==sizeof(s)&&s.version==1&&s.bytes==sizeof(s);}
static void snapshot(const char* label,const power_diag::Sample& s){
    using namespace power_diag;
    put("\n[");put(label);put("]\n");field("uptime_ms",s.ticks);
    for(unsigned i=0;i<Count;++i){
        if(s.valid&(1u<<i))hexfield(names[i],s.msr[i]);
        else {put(names[i]);put("=unavailable\n");}
    }
    if(s.valid&(1u<<HwpRequest)){
        uint64_t r=s.msr[HwpRequest];field("hwp_min",r&255);field("hwp_max",(r>>8)&255);
        field("hwp_desired",(r>>16)&255);
        if(s.features6a&(1u<<10))field("hwp_energy_preference",(r>>24)&255);
        field("hwp_package_control",(r>>42)&1);
    }
    if(s.valid&(1u<<HwpCapabilities)){
        uint64_t r=s.msr[HwpCapabilities];field("hwp_highest",r&255);field("hwp_guaranteed",(r>>8)&255);
        field("hwp_efficient",(r>>16)&255);field("hwp_lowest",(r>>24)&255);
    }
    if(s.valid&(1u<<Thermal)){
        uint64_t r=s.msr[Thermal];field("thermal_status_active",r&1);
        if(r&(1ull<<31))field("degrees_below_tjmax",(r>>16)&127);
    }
    if(s.gpu_valid){
        hexfield("gpu_device",s.gpu_device);hexfield("gpu_rpstat1",s.rpstat);hexfield("gpu_rpnswreq",s.rp_request);
        field("gpu_actual_mhz",gen9_mhz(s.rpstat));field("gpu_requested_mhz",gen9_mhz(s.rp_request));
    }
    else put("gpu_frequency_status=unavailable\n");
}
static void interval(const char* label,const power_diag::Sample& a,const power_diag::Sample& b){
    using namespace power_diag;
    put("\n[");put(label);put("]\n");field("elapsed_ms",b.ticks-a.ticks);
    field("scene_compositions",b.scene_count-a.scene_count);field("scene_pixels",b.scene_pixels-a.scene_pixels);
    field("gpu_flips",(uint32_t)(b.gpu_flips-a.gpu_flips));field("bcs_batches",b.bcs_batches-a.bcs_batches);
    field("rcs_draws",b.rcs_draws-a.rcs_draws);field("repair_pixels",b.repair_pixels-a.repair_pixels);
    unsigned mask=(1u<<Mperf)|(1u<<Aperf);
    if((a.valid&b.valid&mask)==mask){
        uint64_t m=b.msr[Mperf]-a.msr[Mperf],p=b.msr[Aperf]-a.msr[Aperf];
        field("mperf_delta",m);field("aperf_delta",p);
        // APERF/MPERF measures active CPU frequency, not invariant TSC speed.
        if(m&&b.nominal_mhz&&p<=UINT64_MAX/b.nominal_mhz)field("active_cpu_mhz",p*b.nominal_mhz/m);
        else put("active_cpu_mhz=unavailable\n");
    }else put("active_cpu_mhz=unavailable\n");
}
static int run(){
    using power_diag::Sample;
    char path[256];if(call(215,(uint64_t)path,sizeof(path))<=0)return 1;
    print("Power diagnostic: observing 2 seconds, then a short CPU workload.\n");
    static Sample a{},b{},c{};
    if(!sample(a)){print("Power diagnostic unavailable.\n");return 1;}
    call(35,2000);
    if(!sample(b))return 1;
    // Fixed dependent integer work, split into blocks so other apps continue
    // to schedule. This stage is deliberately a CPU load, not a passive trace.
    uint64_t work=1;
    for(unsigned block=0;block<32;++block){
        for(unsigned i=0;i<1000000;++i){work=work*6364136223846793005ull+1;__asm__ volatile("":"+r"(work));}
        call(35,1);
    }
    if(!sample(c))return 1;
    put("MyOS power report v1\npath=");put(path);put("\nPower source is the operator's label; it is not detected automatically.\n");
    put("MSR reads only; no clock, limit, boost or energy policy writes.\n");
    put("Thermal/limit log bits may reflect earlier activity; raw values are included.\n");
    put("GPU counters are cumulative deltas; optional timing instrumentation is not required.\n");
    put("vendor=");put(a.vendor);put("\n");hexfield("cpu_signature",a.signature);field("hypervisor",a.hypervisor);
    hexfield("cpuid6_eax",a.features6a);hexfield("cpuid6_ecx",a.features6c);
    field("nominal_cpu_mhz",a.nominal_mhz);field("invariant_tsc_hz",a.tsc_hz);
    field("work_iterations",32000000);field("work_checksum",work);
    snapshot("before",a);snapshot("after_observation",b);snapshot("after_cpu_work",c);
    interval("observation",a,b);interval("cpu_work_including_scheduling",b,c);
    call(83,(uint64_t)"/home",0755);
    int64_t fd=call(2,(uint64_t)path,0x241);
    bool ok=fd>=0;
    for(unsigned at=0;ok&&at<used;){int64_t n=call(1,fd,(uint64_t)(report+at),used-at);if(n<=0||(uint64_t)n>used-at)ok=false;else at+=n;}
    if(ok)ok=call(74,fd)==0;
    if(fd>=0)call(3,fd);
    print(ok?"Saved power report: ":"Cannot save power report: ");print(path);print("\n");
    return ok?0:1;
}
extern "C" __attribute__((noreturn)) void _start(){int result=run();call(60,result);for(;;){}}
