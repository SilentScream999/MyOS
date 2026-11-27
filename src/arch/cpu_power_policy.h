#ifndef WYZAR_CPU_POWER_POLICY_H
#define WYZAR_CPU_POWER_POLICY_H
#include <stdint.h>
#include "../drivers/power_diag.h"

// Repair a minimum legacy request inherited at boot on Skylake/Kaby Lake
// clients. This is not a periodic governor: only the ratio field may change,
// once, on the BSP (the only CPU currently used by MyOS).
namespace cpu_power {
struct Features { bool intel,hypervisor,speedstep;uint32_t signature,thermal_features; };
enum class Status { Unsupported,VirtualMachine,SpeedstepDisabled,HwpActive,
    ReadFailed,InvalidRatios,Unchanged,Applied,WriteFailed,VerifyFailed };
struct Result {
    Status status=Status::Unsupported;
    uint64_t before=0,requested=0,after=0;
    unsigned minimum=0,base=0;
};
template<class IO> Result initialize(const Features& f,IO& io){
    Result result{};
    if(!f.intel||!power_diag::client_model(f.signature))return result;
    if(f.hypervisor){result.status=Status::VirtualMachine;return result;}
    if(!f.speedstep){result.status=Status::SpeedstepDisabled;return result;}
    uint64_t enable=0,misc=0,platform=0;
    result.status=Status::ReadFailed;
    if(f.thermal_features&(1u<<7)){
        if(!io.read(0x770,enable))return result;
        if(enable&1){result.status=Status::HwpActive;return result;}
    }
    if(!io.read(0x1a0,misc))return result;
    if(!(misc&(1ull<<16))){result.status=Status::SpeedstepDisabled;return result;}
    if(!io.read(0xce,platform)||!io.read(0x199,result.before))return result;
    result.minimum=(platform>>40)&255;result.base=(platform>>8)&255;
    if(!result.minimum||result.base<=result.minimum){result.status=Status::InvalidRatios;return result;}
    result.requested=result.after=result.before;
    if(((result.before>>8)&255)>result.minimum){result.status=Status::Unchanged;return result;}
    // Keep every other field, including bit 32 (turbo disengage), unchanged.
    result.requested=(result.before&~0xff00ull)|((uint64_t)result.base<<8);
    if(!io.write(0x199,result.requested)){result.status=Status::WriteFailed;return result;}
    if(!io.read(0x199,result.after)||((result.after^result.requested)&0xff00)){
        result.status=Status::VerifyFailed;return result;
    }
    result.status=Status::Applied;return result;
}
}
#endif
