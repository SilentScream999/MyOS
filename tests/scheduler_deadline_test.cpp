#include <cassert>
#include <cstdio>
#include "src/core/scheduler_deadline.h"
enum class State { READY,RUNNING,BLOCKED,DEAD };
struct Task { unsigned pid;State state;uint64_t wake_at_tick;Task* next; };
int main() {
    Task idle{0,State::RUNNING,0,nullptr},compositor{1,State::BLOCKED,5,nullptr},app{2,State::BLOCKED,5,nullptr};
    idle.next=&compositor;compositor.next=&app;app.next=&idle;
    assert(!scheduler_wake_compositor_deadline<Task>(nullptr,5));
    assert(!scheduler_wake_compositor_deadline(&idle,4));
    assert(scheduler_wake_compositor_deadline(&idle,5));
    assert(compositor.state==State::READY&&compositor.wake_at_tick==0);
    assert(app.state==State::BLOCKED&&app.wake_at_tick==5);
    assert(!scheduler_wake_compositor_deadline(&idle,6));
    compositor.state=State::BLOCKED;assert(!scheduler_wake_compositor_deadline(&idle,100)); // event wait
    compositor.state=State::DEAD;compositor.wake_at_tick=4;
    assert(!scheduler_wake_compositor_deadline(&idle,100));
    // Serial one-ms GPU sleeps can resume every timer tick. Ordinary time
    // slices still expire at 10,20,... rather than being postponed by wakes.
    uint32_t quantum=0;
    for(unsigned tick=1;tick<=100;++tick){
        compositor.state=State::BLOCKED;compositor.wake_at_tick=tick;
        bool due=scheduler_wake_compositor_deadline(&idle,tick);
        assert(due&&scheduler_tick_due(quantum,due,10));
        assert(quantum==tick%10);
    }
    quantum=0;
    for(unsigned tick=1;tick<=100;++tick)
        assert(scheduler_tick_due(quantum,false,10)==(tick%10==0));
    puts("Compositor one-tick deadlines, manual waits, dead tasks and ordinary scheduler quanta passed.");
}
