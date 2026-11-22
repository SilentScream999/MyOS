#ifndef WYZAR_GPU_GEN9_WORKAROUNDS_H
#define WYZAR_GPU_GEN9_WORKAROUNDS_H
#include <stdint.h>

// SKL/KBL Gen9 Core. Match Linux v6.8 intel_workarounds.c
// gen9_wa_init_mcr() and rcs_engine_wa_init(). Call after engine reset,
// with GT/render forcewake held, before submitting the first context.
template<class IO>
static bool gpu_apply_gen9_engine_workarounds(IO& io,uint32_t fuse2) {
    unsigned slice=0,subslice=0;
    while(slice<3 && !(fuse2&(1u<<(25+slice))))++slice;
    while(subslice<4 && (fuse2&(1u<<(20+subslice))))++subslice;
    if(fuse2==UINT32_MAX || slice==3 || subslice==4)return false;
    uint32_t steering=io.read(0xFDC);
    if(steering==UINT32_MAX)return false;
    constexpr uint32_t steering_mask=0x0F000000;
    uint32_t selected=(slice<<26)|(subslice<<24);
    io.write(0xFDC,(steering&~steering_mask)|selected);
    if((io.read(0xFDC)&steering_mask)!=selected)return false;

    struct Update {uint32_t reg,clear,set;};
    const Update updates[]={
        {0xB004,0,1u<<7}, // WaEnableGapsTsvCreditFix
        {0xB008,1u,0}, // Disable non-IA coherent L3 atomics
        {0xB118,1u<<22,1u<<21}, // WaOCLCoherentLineFlush, disable L3 atomics
        {0xB11C,1u<<8,1u<<2}, // LBS retry decrement, disable eviction perf fix
    };
    for(const auto& update:updates){
        uint32_t before=io.read(update.reg);
        if(before==UINT32_MAX)return false;
        io.write(update.reg,(before&~update.clear)|update.set);
        uint32_t mask=update.clear|update.set;
        if((io.read(update.reg)&mask)!=update.set)return false;
    }
    // WaContextSwitchWithConcurrentTLBInvalidate. This register uses
    // upper-half write enables, unlike the read/modify/write registers above.
    io.write(0x20D4,0x00040004);
    return (io.read(0x20D4)&4)!=0;
}
#endif
