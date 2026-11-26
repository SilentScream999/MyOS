// Native, bounded-memory MP3 player. Decode work and PCM submission are
// incremental so the same thread can service windows and the common picker.
#include "native_ui.h"
#include "native_file_dialog.h"
#include "native_audio.h"
#include "audio_resampler.h"
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
extern "C" {
#include "third_party/minimp3/minimp3.h"
}
#ifndef NATIVE_HOST_TEST
extern "C" void* memmove(void* d,const void* s,size_t n) {
    if((uintptr_t)d<(uintptr_t)s)return memcpy(d,s,n);
    for(size_t i=n;i;--i)((uint8_t*)d)[i-1]=((const uint8_t*)s)[i-1];return d;
}
extern "C" int memcmp(const void* a,const void* b,size_t n) {
    for(size_t i=0;i<n;++i)if(((const uint8_t*)a)[i]!=((const uint8_t*)b)[i])return ((const uint8_t*)a)[i]-((const uint8_t*)b)[i];return 0;
}
#endif
static NativeSurface surface{};
static NativeFileDialog picker;
static uint64_t window_id;
static char path[256],message[192]="Open an MP3 file to get started.";
static bool dirty=true,exiting=false,paused=false,stream=false,eof=false,finished=false;
static int64_t file=-1;
static uint8_t input[65536];
static int16_t decoded[MINIMP3_MAX_SAMPLES_PER_FRAME],output[8192];
static unsigned remaining=0,pending=0,sent=0,seconds=0;
static uint64_t decoded_frames=0;
static int mixer=100,hovered=-1;
static bool volume_dragging=false;
static void publish_volume(){if(stream)NativeAudio::control(NativeAudio::StreamVolume,mixer&255,bool(mixer&256));}
static mp3dec_t decoder;
static AudioResampler resampler{};
static const NativeToolbarButton open_button={12,10,100,32,NativeIcon::Open,"Open...","Open MP3 (Ctrl+O)"};
static void note(const char* value){native_copy(message,sizeof(message),value);dirty=true;}
static void release_audio(){
    // The service may briefly own the device while applying a mixer update.
    // Do not forget an owned stream until its close has actually run.
    if(stream)while(NativeAudio::control(NativeAudio::Close)==-2)native_call(35,2);
    if(file>=0)sys_close(file);
    file=-1;stream=false;paused=false;remaining=pending=sent=0;finished=false;
}
static void stop(){release_audio();seconds=0;note(path[0]?"Stopped. Press Play to restart.":"Open an MP3 file to get started.");}
static bool load(const char* candidate){
    file_info info{};
    if(candidate[0]!='/'||native_length(candidate)>=sizeof(path)||!native_music_file(candidate)||
       sys_stat(candidate,&info)!=0||info.type!=1||!info.size){note("Choose a readable MP3 file.");return false;}
    int64_t next=(int64_t)sys_open(candidate,0);
    if(next<0){note("Cannot open that MP3 file.");return false;}
    uint8_t header[10];unsigned done=0;
    while(done<10){int64_t n=(int64_t)sys_read(next,(char*)header+done,10-done);
        if(n<0||n>(int64_t)(10-done)){sys_close(next);note("Cannot read MP3 file.");return false;}
        if(!n)break;done+=(unsigned)n;}
    uint64_t start=0;
    if(done==10&&!memcmp(header,"ID3",3)){
        if((header[6]|header[7]|header[8]|header[9])&128){sys_close(next);note("Invalid ID3 tag size.");return false;}
        start=10+((uint32_t)header[6]<<21)+((uint32_t)header[7]<<14)+((uint32_t)header[8]<<7)+header[9];
    }
    if(start>=info.size||sys_lseek(next,start,0)<0){sys_close(next);note("MP3 contains no audio data.");return false;}
    release_audio();file=next;native_copy(path,sizeof(path),candidate);
    eof=false;finished=false;decoded_frames=0;seconds=0;resampler={};mp3dec_init(&decoder);
    note("Opening audio output...");return true;
}
static bool emit(int16_t left,int16_t right){
    if(pending+2>sizeof(output)/sizeof(output[0]))return false;
    output[pending++]=left;output[pending++]=right;return true;
}
static void fail(const char* value){release_audio();note(value);}
static void tick_audio(){
    if(file<0||paused)return;
    // At most sixteen frames per pass, with bounded reads and no wait for DMA
    // space. Even corrupt/junk files yield to input between passes.
    for(unsigned work=0;work<16;++work){
        if(pending){
            int64_t n=NativeAudio::control(NativeAudio::TryWrite,(uint64_t)(output+sent),(pending-sent)*2);
            if(n==-2||n==0)return;
            if(n<0||n>(int64_t)((pending-sent)*2)||(n&3)){fail("Audio output failed. Check Boot Log, then press Play to retry.");return;}
            sent+=(unsigned)n/2;if(sent<pending)return;pending=sent=0;
        }
        if(finished){
            int64_t queued=NativeAudio::control(NativeAudio::Queued,1);
            if(queued==-2)return;
            if(queued<0){fail("Could not finish audio playback.");return;}
            if(!queued){release_audio();note("Playback finished. Press Play to listen again.");}
            return;
        }
        if(!eof&&remaining<16384){
            int64_t n=(int64_t)sys_read(file,(char*)input+remaining,sizeof(input)-remaining);
            if(n<0||n>(int64_t)(sizeof(input)-remaining)){fail("Could not read MP3 file.");return;}
            if(!n)eof=true;else remaining+=(unsigned)n;
        }
        if(!remaining){
            if(!decoded_frames){fail("No decodable MP3 audio found.");return;}
            if(!resampler.finish(emit)){fail("Audio conversion failed.");return;}
            finished=true;continue;
        }
        // Open before consuming the first frame, so a transient busy response
        // cannot discard decoded samples or change decoder history.
        mp3dec_frame_info_t info{};
        mp3dec_t next=decoder;
        int samples=mp3dec_decode_frame(&next,input,remaining,decoded,&info);
        if(samples>0){
            if(info.layer!=3||(info.channels!=1&&info.channels!=2)||info.hz<8000||info.hz>48000){fail("Unsupported MP3 audio format.");return;}
            if(!stream){
                int64_t rate=NativeAudio::control(NativeAudio::Open,0,info.hz==44100?44100:48000);
                if(rate==-2){fail("Audio output is busy. Stop the other player, then press Play.");return;}
                if(rate!=44100&&rate!=48000){fail("No supported audio output is available.");return;}
                stream=true;resampler.rate=info.hz;resampler.target=(unsigned)rate;note("Playing MP3. Space: pause/resume. S: stop.");
                publish_volume();
            }else if(resampler.rate!=(unsigned)info.hz){fail("MP3 sample rate changed during playback.");return;}
            for(int i=0;i<samples;++i)if(!resampler.push(decoded[i*info.channels],decoded[i*info.channels+(info.channels==2?1:0)],emit)){
                fail("Audio conversion failed.");return;}
            decoded_frames+=(unsigned)samples;
        }
        decoder=next;
        if(info.frame_bytes<=0){
            if(eof)remaining=0;
            else if(remaining>=16384){memmove(input,input+remaining-4096,4096);remaining=4096;}
        }else {remaining-=info.frame_bytes;memmove(input,input+info.frame_bytes,remaining);}
    }
}
static void toggle_play(){
    if(file<0){if(path[0])load(path);else note("Use Open to choose an MP3 file.");return;}
    if(!stream){stop();return;}
    int64_t result=NativeAudio::control(NativeAudio::Pause,paused?0:1);
    if(result==-2)return;
    if(result<0){fail("Could not change playback state.");return;}
    paused=!paused;note(paused?"Paused. Space: resume.":"Playing MP3. Space: pause/resume. S: stop.");
}
static void open_picker(){volume_dragging=false;hovered=-1;dirty=true;if(!picker.show(false,path[0]?path:"/home/"))note("Could not open file browser.");}
static NativeToolbarButton play_button(){return {surface.w/2-86,surface.h-144,100,42,paused||file<0?NativeIcon::Play:NativeIcon::Pause,paused||file<0?"Play":"Pause","Play / pause (Space)"};}
static NativeToolbarButton stop_button(){return {surface.w/2+26,surface.h-144,64,42,NativeIcon::Stop,nullptr,"Stop (S)"};}
static void set_volume(int value){if(value<0)value=0;if(value>100)value=100;
    if((mixer&255)==value)return;
    mixer=(mixer&256)|value;publish_volume();dirty=true;}
static void toggle_mute(){mixer^=256;publish_volume();dirty=true;}
static int volume_left(){return surface.w/2-64;}
static int volume_percent(int pointer){return (pointer-volume_left())*100/172;}
static void render(){
    surface.rect(0,0,surface.w,surface.h,0xff171923);
    surface.rect(0,0,surface.w,52,0xff242334);open_button.draw(surface,true,hovered==0);
    surface.text(130,19,"MP3 audio",0xffb3aec5,surface.w-12,12);
    int art_y=70,art_size=surface.h-330;if(art_size<56)art_size=56;if(art_size>160)art_size=160;
    surface.rounded((surface.w-art_size)/2,art_y,art_size,art_size,16,0xff302d46);
    native_draw_icon(surface,NativeIcon::Music,(surface.w-art_size)/2+art_size/5,art_y+art_size/5,art_size*3/5,0xffc6b3f3);
    const char* name=path[0]?path:"Your music, ready to play";for(const char* p=name;*p;++p)if(*p=='/')name=p+1;
    surface.centered_text(12,art_y+art_size+14,surface.w-24,30,name,0xffeeebfa,18);
    surface.centered_text(12,art_y+art_size+46,surface.w-24,22,paused?"Paused":file>=0?(stream?"Now playing":"Loading..."):path[0]?"Ready to replay":"Choose an MP3 from your files",0xffb3aec5,12);
    char minutes[24],time[32];native_number(seconds/60,minutes);unsigned n=native_length(minutes);
    native_copy(time,sizeof(time),minutes);time[n++]=':';time[n++]='0'+seconds%60/10;time[n++]='0'+seconds%10;time[n]=0;
    surface.centered_text(12,surface.h-180,surface.w-24,24,time,0xffd5c8ed,14);
    play_button().draw(surface,path[0],hovered==1);stop_button().draw(surface,file>=0,hovered==2);
    int x=surface.w/2-140,y=surface.h-74;
    surface.text(x,y-4,(mixer&256)?"Unmute":"Mute",0xffc6bfd8,x+66,12);
    surface.rounded(x+76,y+1,172,6,3,0xff45425b);
    int volume=mixer&255;surface.rounded(x+76,y+1,volume*172/100,6,3,0xffbba4f4);
    surface.rounded(x+69+volume*172/100,y-3,14,14,7,0xffeee7ff);
    char percent[24];native_number(volume,percent);surface.text(x+260,y-4,percent,0xffc6bfd8,surface.w-12,12);
    surface.rect(0,surface.h-28,surface.w,28,0xff242334);
    surface.text(12,surface.h-21,message,0xffc6bfd8,surface.w-12,12);
    dirty=(int64_t)native_call(222,window_id)<0;
}
static void handle_event(const NativeEvent& e){
    if(e.type==7){volume_dragging=false;exiting=true;return;}
    if(e.type==8){volume_dragging=false;if(native_accept_resize(window_id,surface))dirty=true;return;}
    if(e.type==2){if(e.key&1)volume_dragging=false;return;}
    if(picker.window)return;
    if(e.type==5){if((e.mods&1)&&(e.key=='o'||e.key=='O'))open_picker();
        else if(e.key==' ')toggle_play();else if(e.key=='s'||e.key=='S')stop();
        else if(e.key=='m'||e.key=='M')toggle_mute();
        else if(e.key=='+'||e.key=='=')set_volume((mixer&255)+5);else if(e.key=='-')set_volume((mixer&255)-5);return;}
    if(e.type==4){set_volume((mixer&255)+(e.delta>0?5:-5));return;}
    if(e.type==3){if(volume_dragging){
            if(e.key&1){set_volume(volume_percent(e.x));return;}
            volume_dragging=false; // Release may have occurred outside this client.
        }
        int next=open_button.hit(e.x,e.y)?0:play_button().hit(e.x,e.y)?1:stop_button().hit(e.x,e.y)?2:-1;
        if(next!=hovered){hovered=next;dirty=true;}return;}
    if(e.type!=1||e.key!=1)return;
    if(open_button.hit(e.x,e.y))open_picker();else if(play_button().hit(e.x,e.y))toggle_play();
    else if(stop_button().hit(e.x,e.y))stop();
    else if(e.y>=surface.h-84&&e.y<surface.h-54){int x=surface.w/2-140;
        if(e.x>=x&&e.x<x+66)toggle_mute();
        else if(e.x>=x+69&&e.x<=x+255){volume_dragging=true;set_volume(volume_percent(e.x));}}
}
#ifndef NATIVE_HOST_TEST
extern "C" void _start(){
    window_id=native_call(200,(uint64_t)"Music Player",180,100,640,500);
    if(!window_id){native_call(60,1);for(;;){}}
    surface={(uint32_t*)native_call(203,window_id),640,500};
    if(!surface.pixels){native_call(60,1);for(;;){}}
    native_enable_resize(window_id,480,400,surface);
    char document[256]{};if((int64_t)native_call(215,(uint64_t)document,sizeof(document))>0)load(document);
    unsigned frame=0;
    while(!exiting){NativeEvent e{};unsigned n=0;
        while(n++<64&&native_call(205,window_id,(uint64_t)&e))handle_event(e);
        n=0;while(n++<64&&picker.window&&native_call(205,picker.window,(uint64_t)&e)){
            auto result=picker.event(e,frame);if(result==NativeFileDialog::ACCEPT){if(load(picker.result))picker.hide();else picker.tell(message);}
            else if(result==NativeFileDialog::CANCEL)picker.hide();}
        if(!exiting)tick_audio();
        if(frame%8==0){if(stream){int64_t played=NativeAudio::control(NativeAudio::Played);
                if(played>=0){unsigned now=(uint64_t)played/(resampler.target*4);if(now!=seconds){seconds=now;dirty=true;}}}}
        if(dirty)render();if(picker.window){picker.tick(frame);if(picker.dirty)picker.render();}
        ++frame;native_call(35,file>=0&&!paused?8:32);
    }
    picker.hide();release_audio();native_call(60,0);for(;;){}
}
#endif
