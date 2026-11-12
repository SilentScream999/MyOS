#ifndef WYZAR_CPU_IDLE_H
#define WYZAR_CPU_IDLE_H
#include <stdint.h>

// End a halt interval at interrupt entry, before the handler can switch tasks.
// Finishing the old halt again after its task resumes must not count other work.
struct CpuIdleClock {
    uint64_t started=0;
    bool halted=false;
    void begin(uint64_t now) { started=now; halted=true; }
    uint64_t end(uint64_t now) {
        if (!halted) return 0;
        halted=false;
        return now-started;
    }
};
struct CpuUsageSample {
    uint64_t total=0,idle=0;
    uint32_t percent=0;
    bool initialized=false;
    void update(uint64_t now,uint64_t accumulated_idle,uint64_t interval) {
        if (!initialized) { total=now; idle=accumulated_idle; initialized=true; return; }
        uint64_t elapsed=now-total;
        if (!interval || elapsed<interval) return;
        uint64_t idle_ticks=accumulated_idle-idle;
        if (idle_ticks>elapsed) idle_ticks=elapsed;
        percent=(uint32_t)((elapsed-idle_ticks)*100/elapsed);
        total=now; idle=accumulated_idle;
    }
};
#endif
