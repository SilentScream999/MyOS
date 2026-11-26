#ifndef WYZAR_NATIVE_AUDIO_H
#define WYZAR_NATIVE_AUDIO_H
#include "native_ui.h"
// SYS_AUDIO keeps device access in the shared kernel audio service.
namespace NativeAudio {
enum Action : unsigned { Mixer=6, Open=7, Write=8, Drain=9, Close=10,
    Volume=11, Mute=12, TryWrite=13, Pause=14, Queued=15, Played=16, StreamVolume=17 };
static int64_t control(Action action,uint64_t arg=0,uint64_t bytes=0) {
    return (int64_t)native_call(224,action,arg,bytes);
}
}
static bool native_music_file(const char* name) {
    unsigned n=native_length(name);
    return n>4 && name[n-4]=='.' && (name[n-3]=='m'||name[n-3]=='M') &&
        (name[n-2]=='p'||name[n-2]=='P') && name[n-1]=='3';
}
#endif
