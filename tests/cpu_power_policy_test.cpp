#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "src/arch/cpu_power_policy.h"
using namespace cpu_power;
static Features supported(){return {true,false,true,0x806e9,1u<<7};}
struct IO {
    uint64_t enable=0,misc=0x850089,platform=0x4043df1011d00,control=0x400;
    unsigned reads=0,writes=0,fail_read=0;
    bool fail_write=false,ignore_write=false;
    bool read(uint32_t msr,uint64_t& value){
        if(++reads==fail_read)return false;
        switch(msr){case 0x770:value=enable;break;case 0x1a0:value=misc;break;
            case 0xce:value=platform;break;case 0x199:value=control;break;default:assert(false);}
        return true;
    }
    bool write(uint32_t msr,uint64_t value){
        assert(msr==0x199);assert(((value^control)&~0xff00ull)==0);++writes;
        if(fail_write)return false;
        if(!ignore_write)control=value;return true;
    }
};
int main(){
    IO battery;auto result=initialize(supported(),battery);
    assert(result.status==Status::Applied&&result.before==0x400&&result.after==0x1d00);
    assert(result.base==29&&result.minimum==4&&battery.writes==1);
    assert(initialize(supported(),battery).status==Status::Unchanged&&battery.writes==1);
    // Actual AC-boot request and higher/turbo requests must remain untouched.
    for(auto request:{0x1b00ull,0x1d00ull,0x2300ull}){
        IO ac;ac.control=request;assert(initialize(supported(),ac).status==Status::Unchanged);
        assert(ac.writes==0&&ac.control==request);
    }
    // Preserve all unrelated bits for every base/minimum pair and inherited
    // low request, including turbo-disengage bit 32 and reserved fields.
    for(unsigned minimum=1;minimum<16;++minimum)for(unsigned base=minimum+1;base<64;++base){
        IO io;io.platform=((uint64_t)minimum<<40)|((uint64_t)base<<8);
        io.control=0x12345679000000a5ull|((uint64_t)(minimum-1)<<8);
        uint64_t before=io.control;auto r=initialize(supported(),io);
        assert(r.status==Status::Applied&&((io.control>>8)&255)==base);
        assert((io.control&~0xff00ull)==(before&~0xff00ull));
    }
    for(unsigned mode=0;mode<4;++mode){
        auto f=supported();IO io;
        if(mode==0)f.intel=false;if(mode==1)f.signature=0x306a9;
        if(mode==2)f.hypervisor=true;if(mode==3)f.speedstep=false;
        initialize(f,io);assert(io.reads==0&&io.writes==0);
    }
    IO hwp;hwp.enable=1;assert(initialize(supported(),hwp).status==Status::HwpActive&&hwp.writes==0);
    IO disabled;disabled.misc=0;assert(initialize(supported(),disabled).status==Status::SpeedstepDisabled&&disabled.writes==0);
    for(auto platform:{0ull,0x40000000400ull,0x40000000300ull}){
        IO io;io.platform=platform;
        assert(initialize(supported(),io).status==Status::InvalidRatios&&io.writes==0);
    }
    for(unsigned read=1;read<=4;++read){IO io;io.fail_read=read;
        assert(initialize(supported(),io).status==Status::ReadFailed&&io.writes==0);}
    IO failed;failed.fail_write=true;assert(initialize(supported(),failed).status==Status::WriteFailed&&failed.control==0x400);
    IO ignored;ignored.ignore_write=true;assert(initialize(supported(),ignored).status==Status::VerifyFailed&&ignored.writes==1);
    IO unreadable;unreadable.fail_read=5;assert(initialize(supported(),unreadable).status==Status::VerifyFailed&&unreadable.writes==1);
    auto legacy=supported();legacy.thermal_features=0;IO io;
    assert(initialize(legacy,io).status==Status::Applied&&io.reads==4);
    puts("PASS: battery request repair, AC preservation, feature gates, field preservation and failure handling");
}
