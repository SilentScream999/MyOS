#ifndef WYZAR_GPU_RENDER_RESET_H
#define WYZAR_GPU_RENDER_RESET_H
#include <stdint.h>

// Gen8/9 engine reset preparation. A completion fence is not a substitute for
// the engine's ready-for-reset acknowledgement. In particular, an unprepared
// Kaby Lake reset during batch execution can hang the whole system.
// https://codebrowser.dev/linux/linux/drivers/gpu/drm/i915/gt/intel_reset.c.html
// gen8_engine_reset_prepare(), gen8_reset_engines(), gen8_engine_reset_cancel().
// IO supplies read/write/now/pause; timeout is in IO's monotonic clock units.
template<class IO> static bool gpu_render_domain_reset(IO& io,uint64_t timeout) {
    constexpr uint32_t reset_ctl=0x20D0,domain_reset=0x941C;
    constexpr uint32_t request=1,ready=2,render_domain=2;
    for(unsigned pass=0;pass<2;++pass){
        io.write(reset_ctl,(request<<16)|request);
        uint64_t started=io.now();
        while(!(io.read(reset_ctl)&ready)){
            if(io.now()-started>=timeout){
                io.write(reset_ctl,request<<16);
                return false; // Never issue an unprepared domain reset.
            }
            io.pause();
        }
        io.write(domain_reset,render_domain);
        started=io.now();
        while(io.read(domain_reset)&render_domain){
            if(io.now()-started>=timeout){
                io.write(reset_ctl,request<<16);
                return false;
            }
            io.pause();
        }
        // Cancel preparation on every exit, including successful passes.
        io.write(reset_ctl,request<<16);
    }
    return true;
}
#endif
