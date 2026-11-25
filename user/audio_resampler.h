#ifndef WYZAR_AUDIO_RESAMPLER_H
#define WYZAR_AUDIO_RESAMPLER_H
#include <stdint.h>
#include "audio_sinc_table.h"
// Native-rate PCM is untouched. Other rates use a continuous 32-tap windowed
// sinc with interpolated phases, retaining history across MP3 frame boundaries.
struct AudioResampler {
    unsigned rate = 0, target = 48000;
    uint64_t frames = 0, next = 0;
    int16_t history[64][2]{}, first[2]{}, last[2]{};
    int16_t sample(int64_t index, unsigned channel) const {
        if (index < 0) return first[channel];
        if ((uint64_t)index >= frames) return last[channel];
        return history[index % 64][channel];
    }
    template<class Emit> bool output(Emit emit) {
        uint64_t center = next / target;
        unsigned phase = (next % target) * 256 / target;
        unsigned mix = (((next % target) * 256) % target) * 65536 / target;
        int64_t sum[2]{};
        int32_t weight_sum = 0;
        for (unsigned tap = 0; tap < 32; ++tap) {
            int32_t a = audio_sinc[phase][tap], b = audio_sinc[phase+1][tap];
            int32_t weight = a + ((int64_t)(b-a)*mix >> 16);
            weight_sum += weight;
            for (unsigned ch=0; ch<2; ++ch) sum[ch] += (int64_t)sample((int64_t)center + tap - 15, ch)*weight;
        }
        int16_t result[2];
        for(unsigned ch=0;ch<2;++ch) {
            int64_t value = sum[ch]/weight_sum;
            result[ch] = value > 32767 ? 32767 : value < -32768 ? -32768 : value;
        }
        next += rate; return emit(result[0],result[1]);
    }
    template<class Emit> bool push(int16_t left, int16_t right, Emit emit) {
        if (!rate || rate > target || target > 48000) return false;
        if (!frames) { first[0]=left;first[1]=right; }
        last[0]=history[frames%64][0]=left;
        last[1]=history[frames%64][1]=right;
        ++frames;
        if (rate==target) return emit(left,right);
        while (next/target+16 < frames) if (!output(emit)) return false;
        return true;
    }
    template<class Emit> bool finish(Emit emit) {
        if (!frames) return false;
        if(rate==target) return true;
        while(next < frames*target) if(!output(emit))return false;
        return true;
    }
};
#endif
