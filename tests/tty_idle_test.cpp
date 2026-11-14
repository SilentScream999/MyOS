#include <cassert>
#include <cstdint>
#include <cstring>
#include "../src/core/cpu_idle.h"
#define TERMINAL_H
#define scheduler_h
#define TTY_HOST_TEST
enum class TaskState { READY,RUNNING,BLOCKED,DEAD };
struct Task {
    unsigned pid;
    TaskState state;
    Task* next;
    uint64_t wake_at_tick=0;
    bool interrupt_requested=false,waiting_tty_input=false;
    void* console=nullptr;
};
static Task *g_task_list,*g_current_task;
static unsigned g_terminal_shell_pid=1,g_terminal_foreground_pid=0;
static unsigned schedules,halts;
static void (*on_schedule)();
static void schedule() { ++schedules; assert(on_schedule); on_schedule(); }
static void scheduler_idle_halt() { ++halts; assert(false && "Input wake must not be lost"); }
static unsigned g_term_view_offset;
static void term_putchar(char) {}
static void term_flip() {}
static void term_sync_backbuffer() {}
static void term_dirty_all() {}
static void term_write(const char*,size_t) {}
static void term_render_cursor() {}
#include "../src/core/tty.h"

int main() {
    CpuIdleClock clock{}; uint64_t idle=0;
    clock.begin(100); idle+=clock.end(110); // IRQ wakes the CPU.
    // Handler/task work runs for 90 ticks, then a different stack halts.
    clock.begin(200); idle+=clock.end(220);
    idle+=clock.end(900); // Old sleeping stack finally resumes: no double count.
    assert(idle==30);
    CpuUsageSample sample{};
    sample.update(1000,100,1000); assert(sample.percent==0);
    sample.update(1500,200,1000); assert(sample.total==1000);
    sample.update(2000,350,1000); assert(sample.percent==75);
    sample.update(3000,1350,1000); assert(sample.percent==0);
    sample.update(4000,1350,1000); assert(sample.percent==100);
    sample.update(5000,4000,1000); assert(sample.percent==0);

    Task reader{1,TaskState::RUNNING,nullptr}; reader.next=&reader;
    g_current_task=g_task_list=&reader;
    char input[16]{};
    tty_set_raw(true);
    on_schedule=[] {
        assert(g_current_task->state==TaskState::BLOCKED && g_current_task->waiting_tty_input);
        tty_input('x');
        assert(g_current_task->state==TaskState::READY);
    };
    assert(tty_read(input,1)==1 && input[0]=='x' && schedules==1 && halts==0);
    assert(!reader.waiting_tty_input);
    tty_input('y'); auto before=schedules;
    assert(tty_read(input,1)==1 && input[0]=='y' && schedules==before); // Already queued: no sleep.
    assert(tty_read(input,0)==0 && schedules==before);

    // Canonical partial lines must not wake readers until a completed line exists.
    tty_set_raw(false); reader.state=TaskState::BLOCKED; reader.waiting_tty_input=true;
    tty_input('a'); assert(reader.state==TaskState::BLOCKED);
    tty_input('\n'); assert(reader.state==TaskState::READY);
    assert(tty_read(input,sizeof(input))==2 && input[0]=='a' && input[1]=='\n');

    // Start-menu requests must wake the registered shell even without a key.
    tty_set_raw(true); reader.state=TaskState::RUNNING;
    on_schedule=[] { assert(tty_queue_launch("/bin/counter.elf")); };
    assert(tty_read(input,1)==0 && tty_shell_launch_ready());
    assert(!reader.waiting_tty_input && !std::strcmp(tty_launch_path,"/bin/counter.elf"));
    tty_launch_pending=false;

    on_schedule=[] { g_current_task->interrupt_requested=true; g_current_task->state=TaskState::READY; };
    assert(tty_read(input,1)==0 && !reader.waiting_tty_input);
    reader.interrupt_requested=false;

    // Multiple readers wake safely; dead and unrelated blocked tasks stay unchanged.
    Task second{2,TaskState::BLOCKED,nullptr,0,false,true};
    Task dead{3,TaskState::DEAD,nullptr,0,false,true};
    Task other{4,TaskState::BLOCKED,&reader};
    reader.next=&second; second.next=&dead; dead.next=&other;
    reader.state=TaskState::BLOCKED; reader.waiting_tty_input=true;
    tty_input('z');
    assert(reader.state==TaskState::READY && second.state==TaskState::READY);
    assert(dead.state==TaskState::DEAD && other.state==TaskState::BLOCKED);
    tty_discard_input(); assert(tty_out_head==tty_out_tail);
}
