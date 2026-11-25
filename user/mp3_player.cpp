#include <stdint.h>
#include <stddef.h>
#include "audio_resampler.h"
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
extern "C" {
#include "third_party/minimp3/minimp3.h"
}

extern "C" void* memcpy(void* d, const void* s, size_t n) {
    for (size_t i=0;i<n;++i) ((uint8_t*)d)[i]=((const uint8_t*)s)[i]; return d;
}
extern "C" void* memmove(void* d, const void* s, size_t n) {
    if ((uintptr_t)d < (uintptr_t)s) return memcpy(d,s,n);
    for (size_t i=n;i;--i) ((uint8_t*)d)[i-1]=((const uint8_t*)s)[i-1]; return d;
}
extern "C" void* memset(void* d, int c, size_t n) {
    for (size_t i=0;i<n;++i) ((uint8_t*)d)[i]=(uint8_t)c; return d;
}
extern "C" int memcmp(const void* a, const void* b, size_t n) {
    for(size_t i=0;i<n;++i) if(((const uint8_t*)a)[i]!=((const uint8_t*)b)[i]) return ((const uint8_t*)a)[i]-((const uint8_t*)b)[i]; return 0;
}
static int64_t call(uint64_t n,uint64_t a=0,uint64_t b=0,uint64_t c=0) {
    int64_t result;
    __asm__ volatile("syscall":"=a"(result):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory"); return result;
}
static void message(const char* text) {
    unsigned n=0;while(text[n])++n;call(1,1,(uint64_t)text,n);
}
static uint8_t input[65536];
static int16_t decoded[MINIMP3_MAX_SAMPLES_PER_FRAME], output[8192];
static mp3dec_t decoder;
extern "C" int play_mp3(const char* path, unsigned kind) {
    int64_t fd=call(2,(uint64_t)path,0);
    if(fd<0){message("Cannot open MP3 file.\n");return -1;}
    int64_t size=call(8,fd,0,2), start=0;
    call(8,fd,0,0);
    int64_t probe=call(0,fd,(uint64_t)input,10);
    if(size<=0||probe<0){message("Cannot read MP3 file.\n");call(3,fd);return -1;}
    if(probe==10&&!memcmp(input,"ID3",3)) {
        if((input[6]|input[7]|input[8]|input[9])&0x80){message("Invalid ID3 tag.\n");call(3,fd);return -1;}
        start=10+((uint32_t)input[6]<<21)+((uint32_t)input[7]<<14)+((uint32_t)input[8]<<7)+input[9];
    }
    if(start>=size||call(8,fd,start,0)<0){message("MP3 file contains no audio.\n");call(3,fd);return -1;}
    mp3dec_init(&decoder);
    AudioResampler resampler{};
    unsigned remaining=0, count=0; bool eof=false, opened=false, ok=true;
    uint64_t frames=0;
    auto flush=[&]() {
        if(!count)return true;
        bool success=call(224,8,(uint64_t)output,count*2)==count*2;
        count=0;return success;
    };
    auto emit=[&](int16_t l,int16_t r) {
        output[count++]=l;output[count++]=r;
        return count<8192||flush();
    };
    while(ok) {
        if(!eof&&remaining<16384) {
            int64_t n=call(0,fd,(uint64_t)(input+remaining),sizeof(input)-remaining);
            if(n<0){ok=false;break;}
            if(!n)eof=true;else remaining+=(unsigned)n;
        }
        if(!remaining)break;
        mp3dec_frame_info_t info{};
        int samples=mp3dec_decode_frame(&decoder,input,remaining,decoded,&info);
        if(info.frame_bytes<=0) {
            if(eof)break;
            // A full buffer with no sync is junk; retain enough bytes for a
            // frame split across reads rather than retrying the same bytes.
            if(remaining>=16384){memmove(input,input+remaining-4096,4096);remaining=4096;}
            continue;
        }
        if(samples>0) {
            if(info.layer!=3||(info.channels!=1&&info.channels!=2)||info.hz<8000||info.hz>48000) {ok=false;break;}
            if(!opened) {
                int64_t actual=call(224,7,kind,info.hz==44100?44100:48000);
                if(actual<0){message("Cannot open audio output (busy or unavailable).\n");ok=false;break;}
                opened=true;resampler.rate=info.hz;resampler.target=actual;
                message(actual==info.hz?"Audio: native-rate playback.\n":"Audio: high-quality resampling.\n");
                message("Playing MP3. Media keys: mute/volume; Ctrl+C: stop.\n");
            } else if(resampler.rate!=(unsigned)info.hz) {message("MP3 sample rate changed midstream.\n");ok=false;break;}
            for(int i=0;i<samples&&ok;++i)
                ok=resampler.push(decoded[i*info.channels],decoded[i*info.channels+(info.channels==2?1:0)],emit);
            frames+=(unsigned)samples;
        }
        remaining-=info.frame_bytes;memmove(input,input+info.frame_bytes,remaining);
    }
    if(ok&&opened)ok=resampler.finish(emit)&&flush()&&call(224,9)==0;
    if(opened)call(224,10);
    call(3,fd);
    if(!frames){message("No decodable MP3 audio found.\n");return -1;}
    message(ok?"Playback finished.\n":"Playback stopped after an audio/decode error. See bootlog.\n");
    return ok?0:-1;
}
extern "C" __attribute__((noreturn)) void _start() {
    char path[256];
    int result=-1;
    if(call(215,(uint64_t)path,sizeof(path))>0) result=play_mp3(path,0);
    else message("Launch with 'play <path.mp3>' from the terminal.\n");
    call(60,result?1:0);for(;;){}
}
