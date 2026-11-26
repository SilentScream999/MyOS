#ifndef WYZAR_AUDIO_VOLUME_H
#define WYZAR_AUDIO_VOLUME_H
#include <stdint.h>
#include "audio_events.h"
struct AudioVolume {
    unsigned percent = 70;
    bool muted = false;
    void key(unsigned code) {
        if (code == 0x20) muted = !muted;
        else {
            if (code == 0x30) percent = percent > 90 ? 100 : percent + 10;
            if (code == 0x2e) percent = percent < 10 ? 0 : percent - 10;
            muted = false;
        }
    }
    unsigned amp(uint32_t caps) const {
        // Attenuation in quarter dB approximates 20*log10(percent/100).
        static constexpr unsigned attenuation[21] = {160,104,80,66,56,48,42,36,32,28,24,21,18,15,12,10,8,6,4,2,0};
        unsigned unity = caps & 127, steps = (caps >> 8) & 127;
        if (unity > steps) unity = steps;
        unsigned down = attenuation[percent / 5] / (((caps >> 16) & 127) + 1);
        unsigned gain = down > unity ? 0 : unity - down;
        return gain | (((muted || !percent) && (caps & 0x80000000u)) ? 0x80 : 0);
    }
};
// IRQ-guarded writers publish desired state; the audio task performs hardware
// transactions later. UI rendering/hit testing never waits for a codec verb.
struct AudioMixer {
    AudioVolume value{};
    uint64_t revision = 1, applied = 0;
    void (*changed)() = nullptr;
    void publish(AudioVolume next) {
        if(next.percent==value.percent&&next.muted==value.muted)return;
        value=next;++revision;if(changed)changed();audio_events::notify();
    }
    void percent(unsigned amount) { auto next=value;next.percent=amount>100?100:amount;publish(next); }
    void mute(unsigned mode) { auto next=value;next.muted=mode==2?!next.muted:bool(mode);publish(next); }
    void key(unsigned code) { auto next=value;next.key(code);publish(next); }
};
static AudioMixer g_audio_mixer{};
#endif
