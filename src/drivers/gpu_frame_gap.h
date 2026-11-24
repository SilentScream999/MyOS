#ifndef WYZAR_GPU_FRAME_GAP_H
#define WYZAR_GPU_FRAME_GAP_H
#include <stdint.h>

enum class GpuGapActivity { Launch, Resources, CatalogPublish, StartOpen, StartClose };

struct GpuGapEvent {
    uint64_t serial=0,begin=0,end=0;
    GpuGapActivity activity{};
    int pid=-1;
    char detail[96]{};

    bool overlaps(uint64_t from,uint64_t to) const {
        return serial&&begin<=to&&(!end||end>=from);
    }
};

// Callers serialize writes and snapshots with local interrupts disabled.
struct GpuGapHistory {
    static constexpr unsigned capacity=64;
    GpuGapEvent events[capacity]{};
    uint64_t latest=0;

    static unsigned slot(uint64_t serial){return (serial-1)%capacity;}
    uint64_t begin(uint64_t tick,GpuGapActivity activity,int pid,const char* detail) {
        auto& event=events[slot(++latest)];
        event.serial=latest;
        event.begin=tick;
        event.end=0;
        event.activity=activity;
        event.pid=pid;
        unsigned i=0;
        if(detail)for(;i+1<sizeof(event.detail)&&detail[i];++i)event.detail[i]=detail[i];
        event.detail[i]=0;
        return latest;
    }
    void finish(uint64_t serial,uint64_t tick) {
        if(!serial)return;
        auto& event=events[slot(serial)];
        if(event.serial==serial)event.end=tick;
    }
};

static inline bool gpu_frame_gap_due(uint64_t interval,uint64_t refreshes,uint64_t hz) {
    return interval&&hz&&((refreshes>=2&&refreshes<0x80000000u)||interval>=hz/30);
}
#endif
