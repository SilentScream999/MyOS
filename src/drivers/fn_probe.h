#ifndef WYZAR_FN_PROBE_H
#define WYZAR_FN_PROBE_H
#include "fn_trace.h"
#include "rtc.h"
namespace fn_probe {
static bool firmware=false,ec_reads=false;
static uint64_t next_sample=0,next_audio=0;
static unsigned nvram_last=256,status_last=256,audio_last=256;
static unsigned sci_last=256,mode_last=256;
static void begin(bool identity,unsigned duration){
    {TtyIrqGuard guard;g_fn_trace.begin(g_tick_count,duration);}
    firmware=identity;ec_reads=identity;next_sample=next_audio=0;
    nvram_last=status_last=audio_last=256;
    sci_last=mode_last=256;
    if(identity){thinkpad_audio::initialize(true);g_fn_trace.flags=1|(thinkpad_audio::supported?2:0);}
    audio_events::notify();
}
static void poll(){
    if(g_fn_trace.active(g_tick_count)){
        bool alive=false;TtyIrqGuard guard;Task* task=g_task_list;
        if(task)do{if(task->pid==g_fn_trace_owner&&task->state!=TaskState::DEAD&&!task->interrupt_requested)alive=true;task=task->next;}while(task!=g_task_list);
        if(!alive)g_fn_trace.deadline=0;
    }
    if(!firmware||!g_fn_trace.active(g_tick_count)||g_tick_count<next_sample)return;
    next_sample=g_tick_count+20;
    // Linux nvram_read_byte(0x60) adds NVRAM_FIRST_BYTE (14): CMOS 0x6e.
    // Read only; reuse RTC's IRQ-guarded index/data transaction.
    unsigned nvram=cmos_read(0x6e);
    if(nvram!=nvram_last){TtyIrqGuard guard;g_fn_trace.append(g_tick_count,1,nvram);nvram_last=nvram;}
    if(!thinkpad_audio::supported||thinkpad_audio::failed)return;
    unsigned sci=inw_acpi(thinkpad_audio::pm1a)&1;
    if(sci!=sci_last){TtyIrqGuard guard;g_fn_trace.append(g_tick_count,6,sci);sci_last=sci;}
    unsigned status=inb(thinkpad_audio::control_port);
    // Record event flags, not transient IBF/OBF changes from our own reads.
    status&=0x60;
    if(status!=status_last){TtyIrqGuard guard;g_fn_trace.append(g_tick_count,2,status);status_last=status;}
    if(ec_reads&&g_tick_count>=next_audio){
        next_audio=g_tick_count+250;
        // Existing bounded/read-only transaction and firmware Global Lock.
        // No EC query 0x84, writes, NVRAM modification or SCI/GPE changes.
        if(thinkpad_audio::busy)return;
        thinkpad_audio::busy=true;
        int value=-1,mode=-1;
        if(thinkpad_audio::lock()){
            auto input=[](uint16_t port){return inb(port);};
            auto output=[](uint16_t port,uint8_t byte){outb(port,byte);};
            auto waiting=[](auto read,unsigned mask,unsigned expected){return thinkpad_audio::wait(read,mask,expected);};
            ThinkpadEcProtocol<decltype(input),decltype(output),decltype(waiting)> protocol{thinkpad_audio::data_port,thinkpad_audio::control_port,input,output,waiting};
            uint8_t byte=0;
            if(protocol.read(0x30,byte)){
                value=byte;
                if(protocol.read(3,byte))mode=byte;
                else {thinkpad_audio::failed=true;ec_reads=false;}
            }
            else {thinkpad_audio::failed=true;ec_reads=false;}
            thinkpad_audio::unlock();
        }else ec_reads=false;
        thinkpad_audio::busy=false;
        if(value<0){TtyIrqGuard guard;g_fn_trace.append(g_tick_count,5,1);}
        else if((unsigned)value!=audio_last){TtyIrqGuard guard;g_fn_trace.append(g_tick_count,3,value);audio_last=value;}
        if(value>=0 && mode<0){TtyIrqGuard guard;g_fn_trace.append(g_tick_count,5,3);}
        if(mode>=0 && (unsigned)mode!=mode_last){TtyIrqGuard guard;g_fn_trace.append(g_tick_count,7,mode);mode_last=mode;}
    }
}
}
#endif
