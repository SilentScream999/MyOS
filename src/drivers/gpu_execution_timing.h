#ifndef WYZAR_GPU_EXECUTION_TIMING_H
#define WYZAR_GPU_EXECUTION_TIMING_H
#include <stdint.h>

// Lower 32-bit free-running engine clocks. Sample intervals are bounded well
// below one wrap. No clock frequency is assumed from a GPU generation name.
static inline uint64_t gpu_timestamp_frequency(uint32_t first,uint32_t last,
        uint64_t cpu_ticks,uint64_t cpu_hz){
    if(!cpu_ticks||!cpu_hz||cpu_hz>10000000000ULL)return 0;
    uint64_t ticks=(uint32_t)(last-first);
    if(ticks>100000000)return 0;
    uint64_t hz=ticks*cpu_hz/cpu_ticks;
    return hz>=1000000&&hz<=100000000?hz:0;
}
static inline uint64_t gpu_timestamp_clock_agreement(uint64_t a,uint64_t b){
    if(!a||!b)return 0;
    uint64_t lower=a<b?a:b,upper=a>b?a:b;
    return upper-lower<=lower/20?(a+b)/2:0;
}
struct GpuExecutionTiming {
    uint64_t samples=0,invalid=0,queue_us=0,setup_us=0,parse_us=0,rt_us=0,drain_us=0,tail_us=0;
    uint64_t observed_us=0;
};
// Stamps are GPU start, pre-draw, parsed, RT/scoreboard drain, final drain.
// They describe command milestones, not isolated shader execution or GPU load.
static inline bool gpu_execution_record(GpuExecutionTiming& out,uint64_t hz,
        uint32_t submitted,const uint32_t stamps[5],uint32_t observed,
        uint64_t cpu_ticks,uint64_t cpu_hz){
    if(!hz||hz>100000000||!cpu_hz||cpu_hz>10000000000ULL||!cpu_ticks||cpu_ticks>cpu_hz){++out.invalid;return false;}
    uint32_t points[7]={submitted,stamps[0],stamps[1],stamps[2],stamps[3],stamps[4],observed};
    uint64_t total=(uint32_t)(observed-submitted),sum=0,parts[6]{};
    if(total>hz/2){++out.invalid;return false;}
    for(unsigned i=0;i<6;++i){parts[i]=(uint32_t)(points[i+1]-points[i]);sum+=parts[i];}
    uint64_t elapsed=cpu_ticks*1000000/cpu_hz,gpu_elapsed=total*1000000/hz;
    uint64_t difference=elapsed>gpu_elapsed?elapsed-gpu_elapsed:gpu_elapsed-elapsed;
    if(sum!=total||difference>2000+elapsed/5){++out.invalid;return false;}
    uint64_t* fields[]={&out.queue_us,&out.setup_us,&out.parse_us,&out.rt_us,&out.drain_us,&out.tail_us};
    for(unsigned i=0;i<6;++i)*fields[i]+=parts[i]*1000000/hz;
    out.observed_us+=elapsed;++out.samples;return true;
}
#endif
