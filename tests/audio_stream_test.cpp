#include <cassert>
#include <vector>
#include <cmath>
#include "src/drivers/hda_pcm_ring.h"
#include "src/drivers/audio_volume.h"
#include "user/audio_resampler.h"
int main() {
    for (unsigned rate : {8000,11025,12000,16000,22050,24000,32000,44100,48000}) {
        AudioResampler r{}; r.rate=rate;
        std::vector<int16_t> left, right;
        auto emit=[&](int16_t l,int16_t rr){left.push_back(l);right.push_back(rr);return true;};
        for(unsigned i=0;i<rate;++i)assert(r.push(1234,-2345,emit));
        assert(r.finish(emit)); assert(left.size()==48000&&right.size()==48000);
        for(unsigned i=0;i<48000;++i)assert(left[i]==1234&&right[i]==-2345);
    }
    AudioResampler ramp{};ramp.rate=ramp.target=44100;
    std::vector<int> values;
    auto emit=[&](int16_t l,int16_t r){assert(l==-r);values.push_back(l);return true;};
    assert(ramp.push(0,0,emit)&&ramp.push(1000,-1000,emit)&&ramp.push(2000,-2000,emit)&&ramp.finish(emit));
    assert((values==std::vector<int>{0,1000,2000})); // Native PCM stays exact.
    AudioResampler treble{};treble.rate=44100;
    std::vector<int16_t> high;
    auto high_emit=[&](int16_t l,int16_t r){assert(l==r);high.push_back(l);return true;};
    for(unsigned i=0;i<44100;++i){int16_t v=10000*std::sin(2*3.141592653589793*15000*i/44100);assert(treble.push(v,v,high_emit));}
    assert(treble.finish(high_emit)&&high.size()==48000);
    double power=0;for(unsigned i=100;i<47900;++i)power+=(double)high[i]*high[i];
    double rms=std::sqrt(power/47800);
    assert(rms>6900&&rms<7250); // Linear interpolation lost substantial treble.
    AudioVolume volume{};
    for(unsigned p=0;p<=100;++p){volume.percent=p;unsigned value=volume.amp(0x80034a4a);assert((value&127)<=74);assert(bool(value&128)==!p);}
    volume.percent=100;volume.key(0x30);assert(volume.percent==100);
    for(unsigned i=0;i<20;++i)volume.key(0x2e);assert(volume.percent==0);
    volume.key(0x20);assert(volume.muted);volume.key(0x30);assert(!volume.muted&&volume.percent==10);
    HdaPcmRing ring{};assert(ring.space()==ring.capacity-ring.block);
    ring.written=ring.capacity-ring.block;assert(ring.space()==0);
    assert(ring.advance(4096,20));assert(ring.consumed==4096&&ring.space()==4096);
    ring.written+=4096;assert(ring.advance(ring.capacity-4096,1100));assert(ring.consumed==ring.capacity-4096);
    assert(ring.advance(4096,1120));assert(ring.consumed==ring.capacity+4096);
    assert(ring.advance(ring.capacity,1130));assert(ring.position==0);
    assert(!ring.advance(ring.capacity+1,1140));assert(!ring.advance(0,2330));
}
