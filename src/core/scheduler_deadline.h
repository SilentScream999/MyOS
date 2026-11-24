#ifndef WYZAR_SCHEDULER_DEADLINE_H
#define WYZAR_SCHEDULER_DEADLINE_H
#include <stdint.h>

// The scheduler already gives PID 1 (the compositor) priority when READY.
// Check its timed sleep at timer frequency, rather than only at slice expiry.
// Other sleepers and manual/event blocking keep their existing policy.
template<class TaskType>
static inline bool scheduler_wake_compositor_deadline(TaskType* head,uint64_t now) {
    if(!head)return false;
    using State=decltype(head->state);
    auto* task=head;
    do {
        if(task->pid==1){
            if(task->state==State::BLOCKED&&task->wake_at_tick&&now>=task->wake_at_tick){
                task->wake_at_tick=0;task->state=State::READY;return true;
            }
            return false;
        }
        task=task->next;
    }while(task!=head);
    return false;
}
static inline bool scheduler_tick_due(uint32_t& elapsed,bool compositor_due,uint32_t slice) {
    if(++elapsed>=slice){elapsed=0;return true;}
    return compositor_due; // early wake does not restart the normal quantum
}
#endif
