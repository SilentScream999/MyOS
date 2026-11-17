#ifndef WYZAR_TASK_USAGE_H
#define WYZAR_TASK_USAGE_H
#include "src/core/process_info.h"
struct CpuHistory {
    static constexpr unsigned capacity=61;
    struct Sample { uint64_t tick;uint32_t cpu; } samples[capacity]{};
    unsigned head=0,count=0;
    const Sample& at(unsigned index)const{return samples[(head+capacity-count+index)%capacity];}
    void add(uint64_t tick,uint32_t cpu) {
        if(count&&tick<=at(count-1).tick)return;
        if(cpu>1000)cpu=1000;
        samples[head]={tick,cpu};head=(head+1)%capacity;if(count<capacity)++count;
    }
};
struct TaskUsage {
    static constexpr int capacity=256;
    ProcessInfo incoming[capacity]{},previous[capacity]{};
    ProcessTotals incoming_totals{},totals{};
    ProcessMetadata incoming_metadata{};
    uint32_t kinds[capacity]{};
    CpuHistory history;
    uint32_t cpu[capacity]{},total_cpu=0;
    int order[capacity]{},count=0;
    bool initialized=false,sampled=false,failed=false,truncated=false;
    bool collapsed[PROCESS_KIND_COUNT]{};
    int rows[capacity+PROCESS_KIND_COUNT]{},row_count=0,group_count[PROCESS_KIND_COUNT]{};
    void rebuild_rows() {
        row_count=0;
        for(unsigned group=0;group<PROCESS_KIND_COUNT;++group) {
            group_count[group]=0;rows[row_count++]=-1-(int)group;
            for(int j=0;j<count;++j)if(kinds[order[j]]==group) {
                ++group_count[group];if(!collapsed[group])rows[row_count++]=order[j];
            }
        }
    }
    void update(int64_t result) {
        failed=result<0;if(failed)return;
        truncated=result>capacity;int next_count=truncated?capacity:(int)result;
        uint64_t elapsed=incoming_totals.elapsed_cycles-totals.elapsed_cycles;
        sampled=initialized&&elapsed>0;
        if(sampled) {
            uint64_t idle=incoming_totals.idle_cycles-totals.idle_cycles;
            if(idle>elapsed)idle=elapsed;
            total_cpu=(uint32_t)((elapsed-idle)*1000/elapsed);
            history.add(incoming_metadata.tick_ms,total_cpu);
        }
        for(int i=0;i<next_count;++i) {
            cpu[i]=0;order[i]=i;
            kinds[i]=incoming_metadata.kinds[i]<PROCESS_KIND_COUNT?incoming_metadata.kinds[i]:PROCESS_BACKGROUND;
            if(sampled)for(int j=0;j<count;++j)if(previous[j].pid==incoming[i].pid) {
                uint64_t delta=incoming[i].cpu_cycles-previous[j].cpu_cycles;
                if(delta>elapsed)delta=elapsed;
                cpu[i]=(uint32_t)(delta*1000/elapsed);break;
            }
        }
        count=next_count;
        for(int i=0;i<count;++i)previous[i]=incoming[i];
        for(int i=1;i<count;++i){int key=order[i],j=i;
            while(j>0&&(cpu[order[j-1]]<cpu[key] ||
                  (cpu[order[j-1]]==cpu[key]&&previous[order[j-1]].pid>previous[key].pid))) {
                order[j]=order[j-1];--j;
            }order[j]=key;
        }
        totals=incoming_totals;initialized=true;
        rebuild_rows();
    }
};
struct KernelUsage {
    KernelCpuInfo incoming{},previous{};
    uint32_t cpu[KCPU_COUNT]{},calls[KCPU_COUNT]{},total=0;
    bool initialized=false,sampled=false,failed=false;
    void reset(){initialized=false;sampled=false;failed=false;}
    void update(int64_t result) {
        failed=result<0;if(failed)return;
        uint64_t elapsed=incoming.sampled_at-previous.sampled_at;
        sampled=initialized&&elapsed>0;total=0;
        uint64_t ms=incoming.tick-previous.tick,total_delta=0;
        for(unsigned i=0;i<KCPU_COUNT;++i) {
            cpu[i]=calls[i]=0;
            // A lease may expire if the app was suspended. Start a new baseline
            // instead of unsigned underflow presenting a false busy sample.
            if(sampled&&incoming.cycles[i]>=previous.cycles[i]&&incoming.calls[i]>=previous.calls[i]) {
                uint64_t delta=incoming.cycles[i]-previous.cycles[i];if(delta>elapsed)delta=elapsed;
                cpu[i]=(uint32_t)(delta*10000/elapsed);total_delta+=delta;
                if(ms)calls[i]=(uint32_t)((incoming.calls[i]-previous.calls[i])*1000/ms);
            }
        }
        if(sampled){if(total_delta>elapsed)total_delta=elapsed;total=(uint32_t)(total_delta*10000/elapsed);}
        previous=incoming;initialized=true;
    }
};
#endif
