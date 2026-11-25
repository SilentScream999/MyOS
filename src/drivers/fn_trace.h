#ifndef WYZAR_FN_TRACE_H
#define WYZAR_FN_TRACE_H
#include <stdint.h>
// Bounded, explicitly armed input/firmware capture. No port I/O in this type.
struct FnTraceEvent { uint32_t milliseconds; uint16_t value; uint8_t kind,stage; };
struct FnTrace {
    static constexpr unsigned capacity=512;
    uint32_t version=1,count=0,dropped=0,flags=0;
    FnTraceEvent events[capacity]{};
    uint64_t start_tick=0,deadline=0;
    uint8_t stage=0;
    void begin(uint64_t now,unsigned duration){count=dropped=flags=0;start_tick=now;deadline=now+duration;stage=0;}
    bool active(uint64_t now) const {return deadline&&now>=start_tick&&now<deadline;}
    void append(uint64_t now,unsigned kind,unsigned value){
        if(!active(now))return;
        if(count==capacity){++dropped;return;}
        events[count++]={(uint32_t)(now-start_tick),(uint16_t)value,(uint8_t)kind,stage};
    }
    unsigned bytes() const {return 16+count*sizeof(FnTraceEvent);}
};
static FnTrace g_fn_trace{};
static uint32_t g_fn_trace_owner=0;
#endif
