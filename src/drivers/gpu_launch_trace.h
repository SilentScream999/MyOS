#ifndef WYZAR_GPU_LAUNCH_TRACE_H
#define WYZAR_GPU_LAUNCH_TRACE_H
#include <stdint.h>
#ifndef GPU_RCS_ALPHA
#define GPU_RCS_ALPHA 0
#endif
struct GpuLaunchTrace {
    char path[96]{};
    uint64_t started=0,ready=0,created=0,painted=0,sampled_frame=0,displayed=0;
    uint64_t max_interval=0,copied=0,direct=0;
    int64_t pid=-1;
    int window=0;
    bool printed=false;
};
inline GpuLaunchTrace g_gpu_launches[32]{};
inline unsigned g_gpu_launch_count=0;
inline uint64_t g_gpu_launch_last_flip=0;
static inline uint64_t gpu_launch_now(){
    uint32_t lo,hi;__asm__ volatile("rdtsc":"=a"(lo),"=d"(hi)::"memory");
    return ((uint64_t)hi<<32)|lo;
}
static inline GpuLaunchTrace* gpu_launch_begin(const char* path){
    if(!GPU_RCS_ALPHA || g_gpu_launch_count==32)return nullptr;
    auto& trace=g_gpu_launches[g_gpu_launch_count];
    for(unsigned i=0;i<sizeof(trace.path)-1 && path[i];++i)trace.path[i]=path[i];
    trace.started=gpu_launch_now();++g_gpu_launch_count;
    return &trace;
}
static inline void gpu_launch_window(int64_t pid,int window){
    if(!GPU_RCS_ALPHA)return;
    for(unsigned i=0;i<g_gpu_launch_count;++i){
        auto& trace=g_gpu_launches[i];
        if(trace.pid==pid && !trace.window){trace.window=window;trace.created=gpu_launch_now();break;}
    }
}
static inline void gpu_launch_paint(int window){
    if(!GPU_RCS_ALPHA)return;
    for(auto& trace:g_gpu_launches)
        if(trace.window==window && !trace.painted)trace.painted=gpu_launch_now();
}
static inline void gpu_launch_sample(int window,uint64_t frame){
    if(!GPU_RCS_ALPHA)return;
    for(auto& trace:g_gpu_launches)
        if(trace.window==window && trace.painted && !trace.displayed)trace.sampled_frame=frame;
}
static inline void gpu_launch_flip(uint64_t frame,uint64_t tick,bool direct){
    if(!GPU_RCS_ALPHA)return;
    uint64_t previous=g_gpu_launch_last_flip;g_gpu_launch_last_flip=tick;
    for(unsigned i=0;i<g_gpu_launch_count;++i){
        auto& trace=g_gpu_launches[i];
        if(trace.printed || !trace.started)continue;
        uint64_t since=previous>trace.started?previous:trace.started;
        uint64_t interval=tick-since;
        if(interval>trace.max_interval)trace.max_interval=interval;
        if(direct)++trace.direct;else ++trace.copied;
        if(trace.sampled_frame==frame && trace.painted && !trace.displayed)trace.displayed=tick;
    }
}
#endif
