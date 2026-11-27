#ifndef WYZAR_POWER_DIAG_H
#define WYZAR_POWER_DIAG_H
#include <stdint.h>
// Fixed diagnostic ABI, action 5 of syscall 225. Never accepts a register index.
namespace power_diag {
enum Register { Mperf,Aperf,Platform,PerfControl,PerfStatus,ClockModulation,
    Thermal,PackageThermal,MiscEnable,PowerUnits,PackageLimit,LimitReasons,
    HwpEnable,HwpCapabilities,HwpRequest,HwpPackageRequest,Count };
inline constexpr uint32_t msrs[Count]={0xe7,0xe8,0xce,0x199,0x198,0x19a,
    0x19c,0x1b1,0x1a0,0x606,0x610,0x64f,0x770,0x771,0x774,0x772};
inline constexpr const char* names[Count]={"MPERF","APERF","PLATFORM_INFO","PERF_CTL",
    "PERF_STATUS","CLOCK_MODULATION","THERM_STATUS","PACKAGE_THERM_STATUS",
    "MISC_ENABLE","RAPL_POWER_UNIT","PKG_POWER_LIMIT","PERF_LIMIT_REASONS",
    "PM_ENABLE","HWP_CAPABILITIES","HWP_REQUEST","HWP_REQUEST_PKG"};
// Gen9 RPSTAT1 actual ratio and RPNSWREQ requested ratio are bits 31:23;
// 50/3 MHz per unit (Intel i915 intel_gt_regs.h / intel_rps.h).
inline uint32_t gen9_mhz(uint32_t reg){return (((reg>>23)&511)*50+1)/3;}
struct Sample {
    uint32_t version,bytes;char vendor[16];uint32_t signature,features6a,features6c;
    uint32_t hypervisor,nominal_mhz,valid,gpu_valid,gpu_device,rpstat,rp_request;
    uint64_t msr[Count],ticks,tsc,tsc_hz,scene_count,scene_pixels,gpu_flips;
    uint64_t bcs_batches,rcs_draws,repair_pixels;
};
inline bool client_model(uint32_t signature){
    unsigned family=(signature>>8)&15,model=((signature>>4)&15)|((signature>>12)&240);
    return family==6&&(model==0x4e||model==0x5e||model==0x8e||model==0x9e);
}
inline bool allowed(unsigned index,bool intel,bool supported_model,uint32_t a,uint32_t c){
    if(!intel)return false;
    if(index==Mperf||index==Aperf)return c&1;
    if(index==Thermal)return a&1;
    if(index==PackageThermal)return a&(1u<<6);
    if(index==HwpEnable||index==HwpCapabilities||index==HwpRequest)return a&(1u<<7);
    if(index==HwpPackageRequest)return (a&(1u<<7))&&(a&(1u<<11));
    return supported_model;
}
}
#endif
