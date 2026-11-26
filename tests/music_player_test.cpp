#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#define NATIVE_HOST_TEST
#include "../user/music_player.cpp"
static std::map<std::string,std::vector<uint8_t>> files;
static uint64_t offset,queued,played;
static std::string reading;
static bool owned,device_paused,backpressure,read_error,device_error;
static unsigned closes,writes;
static unsigned app_gain=100,app_mute=0,global_changes=0;
static uint64_t native_call(uint64_t n,uint64_t a,uint64_t b,uint64_t c,uint64_t,uint64_t){
    switch(n){
    case 4:if(!files.count((char*)a))return -1;*(file_info*)b={};((file_info*)b)->type=1;((file_info*)b)->size=files[(char*)a].size();return 0;
    case 2:if(!files.count((char*)a))return -1;reading=(char*)a;offset=0;return 10;
    case 8:offset=b;return b;
    case 0:{if(read_error)return -1;auto& bytes=files[reading];uint64_t count=c;
        if(count>bytes.size()-offset)count=bytes.size()-offset;
        memcpy((void*)b,bytes.data()+offset,(size_t)count);offset+=count;return count;}
    case 3:case 35:case 222:return 0;
    case 224:
        if(a==NativeAudio::Open){if(device_error)return -1;assert(!owned);owned=true;queued=played=0;return c;}
        if(a==NativeAudio::TryWrite){assert(owned&&!device_paused);if(device_error)return -1;
            if(backpressure)return 0;unsigned count=c>1024?1024:(unsigned)c;queued+=count;++writes;return count;}
        if(a==NativeAudio::Pause){assert(owned);device_paused=b!=0;return 0;}
        if(a==NativeAudio::Close){assert(owned);owned=false;device_paused=false;++closes;return 0;}
        if(a==NativeAudio::Queued)return queued;
        if(a==NativeAudio::Played)return played;
        if(a==NativeAudio::StreamVolume){assert(owned);app_gain=(unsigned)b;app_mute=(unsigned)c;return 0;}
        if(a==NativeAudio::Volume||a==NativeAudio::Mute){++global_changes;return 0;}
        return 70;
    default:return -1;
    }
}
static void capture(const char* filename){std::ofstream out(filename,std::ios::binary);out<<"P6\n"<<surface.w<<" "<<surface.h<<"\n255\n";
    for(int i=0;i<surface.w*surface.h;++i){auto p=surface.pixels[i];char rgb[]={(char)(p>>16),(char)(p>>8),(char)p};out.write(rgb,3);}}
int main(int argc,char** argv){
    assert(argc==2);std::vector<uint32_t> pixels(640*500);surface={pixels.data(),640,500};render();capture(argv[1]);
    assert(native_music_file("/home/Track.MP3")&&!native_music_file("no.mp3.txt"));
    // Valid MPEG1 Layer III 128kbps/44.1kHz stereo frames with silent payload.
    auto& song=files["/home/Track.MP3"];
    for(int i=0;i<80;++i){size_t start=song.size();song.resize(start+417);song[start]=255;song[start+1]=251;song[start+2]=144;song[start+3]=0;}
    assert(load("/home/Track.MP3"));tick_audio();assert(stream&&decoded_frames&&writes);
    handle_event({1,volume_left()+86,surface.h-74,1,0,0});assert(volume_dragging&&(mixer&255)==50&&app_gain==50);
    handle_event({3,volume_left()+172,surface.h-74,1,0,0});assert((mixer&255)==100&&app_gain==100);
    handle_event({3,volume_left()-500,0,1,0,0});assert((mixer&255)==0&&app_gain==0);
    handle_event({2,volume_left()-500,0,1,0,0});assert(!volume_dragging);
    handle_event({3,volume_left()+86,surface.h-74,0,0,0});assert((mixer&255)==0);
    handle_event({1,volume_left()+86,surface.h-74,1,0,0});assert(volume_dragging);
    handle_event({3,volume_left()+172,surface.h-74,0,0,0});assert(!volume_dragging&&(mixer&255)==50);
    handle_event({5,0,0,'m',0,0});assert((mixer&256)&&app_mute==1);
    handle_event({5,0,0,'m',0,0});set_volume(100);assert(app_gain==100&&!app_mute&&!global_changes);
    auto frames=decoded_frames;auto count=writes;
    toggle_play();assert(paused&&device_paused);tick_audio();assert(decoded_frames==frames&&writes==count);
    assert(!load("/missing.mp3")&&paused&&stream); // Invalid choice preserves playback.
    toggle_play();assert(!paused&&!device_paused);
    backpressure=true;tick_audio();frames=decoded_frames;tick_audio();assert(decoded_frames==frames);backpressure=false;
    for(int i=0;i<10000&&!finished;++i)tick_audio();assert(finished&&stream&&file>=0);
    assert(queued==80ull*1152*4); // Real decoder, partial PCM submissions, no lost samples.
    queued=0;tick_audio();assert(!stream&&file<0&&!owned);
    toggle_play();assert(file>=0);tick_audio();assert(stream);stop();assert(!owned&&file<0&&seconds==0);
    assert(load("/home/Track.MP3"));read_error=true;tick_audio();assert(file<0&&!stream);read_error=false;
    files["/home/junk.mp3"]=std::vector<uint8_t>(100000,42);assert(load("/home/junk.mp3"));
    for(int i=0;i<100&&file>=0;++i)tick_audio();assert(file<0&&!owned);
    files["/home/bad.mp3"]={'I','D','3',4,0,0,255,0,0,0};assert(!load("/home/bad.mp3"));
    assert(load("/home/Track.MP3"));device_error=true;tick_audio();assert(file<0&&!stream);device_error=false;
    auto& low=files["/home/low.mp3"];
    for(int i=0;i<10;++i){size_t start=low.size();low.resize(start+576);low[start]=255;low[start+1]=227;low[start+2]=136;low[start+3]=192;}
    assert(load("/home/low.mp3"));for(int i=0;i<10000&&!finished&&file>=0;++i)tick_audio();
    assert(finished&&stream&&resampler.rate==8000&&resampler.target==48000);
    assert(queued==10ull*576*6*4);queued=0;tick_audio();assert(!owned);
    set_volume(150);assert((mixer&255)==100);set_volume(-50);assert((mixer&255)==0);
    assert(!global_changes);
    surface.w=480;surface.h=400;render();
    assert(closes>=2);puts("Music Player decode, partial writes, pause, drain, cleanup, invalid files and rendering passed.");
}
