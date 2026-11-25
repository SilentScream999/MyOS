#ifndef WYZAR_GPU_ALPHA_TAIL_WAIT_H
#define WYZAR_GPU_ALPHA_TAIL_WAIT_H
#include <stdint.h>
#include "gpu_execution_timing.h"

// Scheduling hints only. Neither a prediction nor a spent polling allowance
// proves GPU completion: callers must still check their fence and retirement.
struct GpuAlphaWaitProfile {
    uint64_t key=0,predicted_us=0;
    unsigned samples=0;
    void select(uint64_t value){
        if(value!=key){key=value;predicted_us=0;samples=0;}
    }
    void observe(const GpuExecutionTiming& timing){
        uint64_t drain=timing.queue_us+timing.setup_us+timing.parse_us+timing.rt_us+timing.drain_us;
        if(!key||timing.samples!=1||timing.invalid||drain<100||drain>100000||drain>timing.observed_us){
            predicted_us=0;samples=0;return;
        }
        predicted_us=drain;
        if(samples<2)++samples;
    }
    bool near_completion(uint64_t elapsed_ticks,uint64_t hz) const {
        if(samples<2||!predicted_us||!hz||hz>10000000000ULL||elapsed_ticks>hz)return false;
        uint64_t elapsed_us=elapsed_ticks*1000000/hz;
        return elapsed_us+125>=predicted_us&&elapsed_us<=predicted_us+250;
    }
};

// One 250-us allowance shared by all eligible RCS submissions in a frame.
// Charge only active poll intervals; ordinary one-ms sleeps never count.
struct GpuAlphaPollBudget {
    uint64_t frame=0,used=0,total_ticks=0,last=0,limit=0,clock_hz=0;
    bool active=false;
    void finish(uint64_t now){
        if(!active)return;
        active=false;
        if(now<last){used=limit;return;}
        uint64_t elapsed=now-last;
        total_ticks+=elapsed;
        used=elapsed>=limit-used?limit:used+elapsed;
    }
    bool poll(uint64_t id,uint64_t now,uint64_t hz,bool near){
        finish(now);
        if(id!=frame){
            frame=id;used=0;clock_hz=hz;
            limit=id&&hz<=10000000000ULL?hz/4000:0;
        }
        if(hz!=clock_hz){used=limit;return false;}
        if(!id||!hz||hz>10000000000ULL||!limit||!near||used>=limit)return false;
        active=true;last=now;return true;
    }
};
#endif
