#ifndef WYZAR_AUDIO_HOTKEYS_H
#define WYZAR_AUDIO_HOTKEYS_H
#include <stdint.h>
#include "audio_events.h"

struct AudioHotkeyQueue {
    uint8_t entries[8]{};
    unsigned head = 0, count = 0, received = 0, dropped = 0;
    uint8_t held = 0, repeating = 0;
    uint64_t repeat_at = 0;
    void queue(unsigned key) {
        ++received;
        if (count == 8) { ++dropped; return; }
        entries[(head + count++) % 8] = (uint8_t)key;
        audio_events::notify();
    }
    // PS/2 multimedia make/break codes. Ordinary function keys belong to apps.
    bool accept(bool extended, uint8_t code, bool repeat, uint64_t now = 0) {
        unsigned key = code & 0x7f;
        if (!extended || (key != 0x20 && key != 0x2e && key != 0x30)) return false;
        if(key!=0x20){
            unsigned bit=key==0x30 ? 2u : 1u;
            if(code&0x80){
                if(!(held&bit))return true; // Orphan/duplicate release has no work to publish.
                held&=~bit;
                if(!(held&bit)&&repeating==key){
                    repeating=(held&2)?0x30:(held&1)?0x2e:0;
                    repeat_at=now+400;
                }
                audio_events::notify();
                return true;
            }
            if(repeat||(held&bit))return true;
            held|=bit;repeating=key;repeat_at=now+400;
        }else if((code&0x80)||repeat)return true;
        queue(key);
        return true;
    }
    void tick(uint64_t now) {
        if(!repeating||now<repeat_at)return;
        queue(repeating);repeat_at=now+80; // No burst after a delayed service.
    }
    uint8_t pop() {
        if (!count) return 0;
        uint8_t key = entries[head]; head = (head + 1) % 8; --count; return key;
    }
};
static AudioHotkeyQueue g_audio_hotkeys{};
#endif
