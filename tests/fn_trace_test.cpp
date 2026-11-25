#include <cassert>
#include <cstddef>
#include "src/drivers/fn_trace.h"
int main(){
    static_assert(sizeof(FnTraceEvent)==8&&offsetof(FnTrace,events)==16);
    FnTrace trace{};trace.append(0,0,0x3b);assert(!trace.count);
    trace.begin(100,1000);trace.stage=3;trace.append(110,0,0xe0);
    assert(trace.count==1&&trace.events[0].milliseconds==10&&trace.events[0].stage==3&&trace.bytes()==24);
    trace.append(1100,0,0x3b);assert(trace.count==1);
    for(unsigned i=0;i<600;++i)trace.append(200,0,i);
    assert(trace.count==512&&trace.dropped==89&&trace.bytes()==4112);
    trace.deadline=0;trace.append(210,0,0);assert(trace.count==512);
    trace.begin(1000,1000);assert(!trace.count&&!trace.dropped&&!trace.flags);
}
