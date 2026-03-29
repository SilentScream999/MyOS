#ifndef scheduler_h
#define scheduler_h

// ── scheduler.h ───────────────────────────────────────────────────────────────
//
// Phase 3 — Round-robin kernel scheduler
//
// ARCHITECTURE OVERVIEW
//
//   switch_context(old, new)
//     Naked asm function.  Saves callee-saved registers + rflags onto old's
//     kernel stack, stores the new RSP in old->saved_rsp, loads new->saved_rsp,
//     optionally switches CR3, then restores and returns into new's context.
//     Callee-saved register frame layout on the stack (low → high address):
//       [rflags][r15][r14][r13][r12][rbx][rbp][return_addr]
//       ↑ saved_rsp
//
//   task_trampoline()
//     Every new task's first "return" from switch_context lands here.
//     Reads entry/arg from current_task, calls entry(arg), then task_exit().
//
//   Ready queue
//     Singly-linked FIFO (g_ready_head / g_ready_tail).  current_task (Running)
//     is NEVER in this list.  Blocked and Dead tasks are also not in the list.
//
//   Idle task
//     Represents kmain's own execution context.  scheduler_init() sets it as
//     current_task without allocating a stack; switch_context saves kmain's live
//     RSP into it on the first yield.  When all user tasks exit, execution
//     returns here into the idle hlt loop.
//
// TIMER INTEGRATION — add one line to your IRQ-0 handler:
//
//   #include "scheduler.h"
//   void your_timer_callback() {
//       ++g_tick_count;      // existing line
//       scheduler_tick();    // ← add this
//   }
//
//   By default scheduler_tick() is cooperative (sets a flag; tasks must yield).
//   Uncomment the schedule() call inside scheduler_tick() for preemptive mode.
//
// QUICK-START EXAMPLE (in kmain, after init_heap()):
//
//   scheduler_init();
//   create_task("hello", [](uint64_t){ print((char*)"hello from task"); }, 0);
//   yield();   // switches to the new task; returns here after it exits
//
// ─────────────────────────────────────────────────────────────────────────────

#include "task.h"
#include "heap.h"
#include "helpers.h"

// ── Globals ───────────────────────────────────────────────────────────────────

static Task  g_task_pool[MAX_TASKS];
static int   g_task_count = 0;
static int   g_next_pid   = 1;

// The currently executing task.  Always non-null after scheduler_init().
static Task* current_task = nullptr;

// Ready queue — FIFO, singly-linked via Task::next.
// current_task (Running) is never in this list.
static Task* g_ready_head = nullptr;
static Task* g_ready_tail = nullptr;

// Set by scheduler_tick(); cleared by yield().
static volatile bool g_need_reschedule = false;

// ── Ready-queue helpers ───────────────────────────────────────────────────────

static void ready_enqueue(Task* t) {
    t->next = nullptr;
    if (g_ready_tail) { g_ready_tail->next = t; g_ready_tail = t; }
    else              { g_ready_head = g_ready_tail = t; }
}

static Task* ready_dequeue() {
    if (!g_ready_head) return nullptr;
    Task* t      = g_ready_head;
    g_ready_head = t->next;
    if (!g_ready_head) g_ready_tail = nullptr;
    t->next      = nullptr;
    return t;
}

// ── switch_context ────────────────────────────────────────────────────────────
//
// SysV AMD64 calling convention on entry:
//   rdi = old_task   (offset 8 = saved_rsp, offset 16 = cr3)
//   rsi = new_task   (offset 8 = saved_rsp, offset 16 = cr3)
//
// Only callee-saved registers (rbp, rbx, r12–r15) and rflags are
// saved/restored.  Caller-saved registers are the caller's responsibility per
// the C ABI — the compiler already handles them at every call site.

__attribute__((naked, noinline))
static void switch_context(Task* /*old_task*/, Task* /*new_task*/) {
    asm volatile(
        // Save old task's callee-saved state onto its kernel stack.
        "push %rbp\n"
        "push %rbx\n"
        "push %r12\n"
        "push %r13\n"
        "push %r14\n"
        "push %r15\n"
        "pushfq\n"
        // old_task->saved_rsp = rsp
        "mov %rsp, 8(%rdi)\n"

        // Load new task's stack.
        // rsp = new_task->saved_rsp
        "mov 8(%rsi), %rsp\n"

        // Optionally switch page table.
        // 0 means "keep current" (all kernel tasks share the kernel PML4 half).
        "mov 16(%rsi), %rax\n"
        "test %rax, %rax\n"
        "jz 1f\n"
        "mov %rax, %cr3\n"
        "1:\n"

        // Restore new task's callee-saved state and return into its context.
        "popfq\n"
        "pop %r15\n"
        "pop %r14\n"
        "pop %r13\n"
        "pop %r12\n"
        "pop %rbx\n"
        "pop %rbp\n"
        "ret\n"
    );
}

// ── Forward declarations ───────────────────────────────────────────────────────

static void task_exit();

// ── task_trampoline ───────────────────────────────────────────────────────────
// Entered via `ret` at the end of switch_context on a brand-new task.
// current_task has already been updated to the new task by schedule(), so
// entry and arg are read directly from it.

static void task_trampoline() {
    void     (*fn)(uint64_t) = current_task->entry;
    uint64_t   a             = current_task->arg;
    fn(a);
    task_exit();
    for (;;) asm volatile("hlt");   // unreachable
}

// ── Idle task ─────────────────────────────────────────────────────────────────

static void idle_fn(uint64_t) { for (;;) asm volatile("hlt"); }

static Task g_idle_task;   // never freed; represents the kmain context

// ── scheduler_init ────────────────────────────────────────────────────────────
// Bootstrap the scheduler from kmain's current execution context.
// Must be called after init_heap().

static void scheduler_init() {
    g_idle_task.pid        = 0;
    g_idle_task.state      = TaskState::Running;
    g_idle_task.saved_rsp  = 0;    // filled on first switch_context away
    g_idle_task.cr3        = 0;
    g_idle_task.stack_base = nullptr;
    g_idle_task.stack_size = 0;
    g_idle_task.entry      = idle_fn;
    g_idle_task.arg        = 0;
    g_idle_task.next       = nullptr;
    const char* n = "idle";
    for (int i = 0; i < 5; i++) g_idle_task.name[i] = n[i];

    current_task = &g_idle_task;
    print((char*)"Scheduler: ready (idle = kmain context).");
}

// ── create_task ───────────────────────────────────────────────────────────────
// Allocate a task from the static pool, build its initial kernel-stack frame,
// and enqueue it as Ready.
//
// Initial stack frame (built by hand to look like a mid-switch_context save):
//
//   stack_top  (16-byte aligned)
//   stack_top -  8   [pad, 0]             ensures RSP ≡ 8 mod 16 at trampoline
//   stack_top - 16   [&task_trampoline]   ret target
//   stack_top - 24   [rbp = 0]
//   stack_top - 32   [rbx = 0]
//   stack_top - 40   [r12 = 0]
//   stack_top - 48   [r13 = 0]
//   stack_top - 56   [r14 = 0]
//   stack_top - 64   [r15 = 0]
//   stack_top - 72   [rflags = 0x202]  ← saved_rsp points here
//
// When switch_context pops 7 regs + executes ret:
//   RSP final = stack_top - 8  (≡ 8 mod 16)   ABI: "just after call" ✓

static Task* create_task(const char* name, void (*entry)(uint64_t), uint64_t arg) {
    if (g_task_count >= MAX_TASKS) {
        print((char*)"create_task: pool full");
        return nullptr;
    }

    Task* t        = &g_task_pool[g_task_count++];
    t->pid         = g_next_pid++;
    t->state       = TaskState::Ready;
    t->cr3         = 0;
    t->entry       = entry;
    t->arg         = arg;
    t->next        = nullptr;
    t->stack_size  = TASK_STACK_SIZE;

    int ni = 0;
    while (name[ni] && ni < 31) { t->name[ni] = name[ni]; ni++; }
    t->name[ni] = '\0';

    t->stack_base = (uint8_t*)kmalloc(TASK_STACK_SIZE);
    if (!t->stack_base) {
        g_task_count--;
        print((char*)"create_task: OOM");
        return nullptr;
    }

    // Build the initial frame.
    uint64_t  stack_top = ((uint64_t)(t->stack_base + TASK_STACK_SIZE)) & ~15ULL;
    uint64_t* frame     = (uint64_t*)(stack_top - 72);

    for (int fi = 0; fi < 9; fi++) frame[fi] = 0;

    frame[0] = 0x202ULL;                    // rflags — IF=1 (interrupts enabled)
    // frame[1..6] = 0                      // r15, r14, r13, r12, rbx, rbp
    frame[7] = (uint64_t)&task_trampoline;  // ret target
    // frame[8] = 0                         // padding (stack_top - 8)

    t->saved_rsp = (uint64_t)frame;

    ready_enqueue(t);

    char str[16];
    print((char*)"Scheduler: task created —");
    print(t->name);
    print((char*)"PID:"); to_str(t->pid, str); print(str);

    return t;
}

// ── schedule ──────────────────────────────────────────────────────────────────
// Dequeue the next ready task and switch to it.
// Re-enqueues the current task as Ready unless its state is Blocked or Dead.
// Returns immediately if the ready queue is empty.

static void schedule() {
    Task* next = ready_dequeue();
    if (!next) return;

    Task* old = current_task;

    if (old->state == TaskState::Running) {
        old->state = TaskState::Ready;
        ready_enqueue(old);
    }
    // Blocked / Dead tasks are NOT re-enqueued here.

    current_task        = next;
    current_task->state = TaskState::Running;
    switch_context(old, next);
    // Execution resumes here when another task eventually switches back to old.
}

// ── yield ─────────────────────────────────────────────────────────────────────
// Cooperatively give up the CPU.  Clears the pending-reschedule flag.
// Call from the main loop, from inside a task, or anywhere a yield is safe.

static void yield() {
    g_need_reschedule = false;
    schedule();
}

// ── task_exit ─────────────────────────────────────────────────────────────────
// Mark the current task Dead and hand the CPU to the next ready task.
// Falls back to the idle (kmain) context if nothing else is ready.
// Stack memory is intentionally kept — add a reaper in a later phase.

static void task_exit() {
    current_task->state = TaskState::Dead;

    Task* dying = current_task;
    Task* next  = ready_dequeue();
    if (!next) next = &g_idle_task;

    current_task        = next;
    current_task->state = TaskState::Running;
    switch_context(dying, next);
    for (;;) asm volatile("hlt");   // unreachable
}

// ── block_task ────────────────────────────────────────────────────────────────
// Suspend the CURRENT task (state → Blocked) and immediately yield.
// The task won't run again until unblock_task() is called on it from elsewhere
// (e.g. from an IRQ handler or another task).

static void block_task() {
    current_task->state = TaskState::Blocked;

    Task* old  = current_task;
    Task* next = ready_dequeue();
    if (!next) next = &g_idle_task;

    current_task        = next;
    current_task->state = TaskState::Running;
    switch_context(old, next);
    // Resumes here once unblock_task() re-enqueues `old` and it is scheduled.
}

// ── unblock_task ──────────────────────────────────────────────────────────────
// Move `t` from Blocked → Ready.  Safe to call from an IRQ handler because it
// only enqueues — it never switches context.

static void unblock_task(Task* t) {
    if (t->state != TaskState::Blocked) return;
    t->state = TaskState::Ready;
    ready_enqueue(t);
}

// ── scheduler_tick ────────────────────────────────────────────────────────────
// Call once per timer tick from your IRQ-0 / PIT handler.
//
// DEFAULT (cooperative): sets g_need_reschedule = true.
//   Tasks must call yield() themselves to actually switch.
//
// PREEMPTIVE: uncomment the schedule() call below.
//   switch_context saves only callee-saved regs; caller-saved regs are already
//   preserved by the C calling convention between the IDT asm stub and this
//   callback, so calling schedule() here is safe.

static void scheduler_tick() {
    g_need_reschedule = true;
    // schedule();   // ← uncomment for preemptive round-robin
}

// ── scheduler_dump ────────────────────────────────────────────────────────────

static const char* _sched_state_name(TaskState s) {
    switch (s) {
        case TaskState::Ready:   return "Ready  ";
        case TaskState::Running: return "Running";
        case TaskState::Blocked: return "Blocked";
        case TaskState::Dead:    return "Dead   ";
        default:                 return "???????";
    }
}

static void scheduler_dump() {
    char str[16];
    print((char*)"── Scheduler dump ────────────────");
    print((char*)"Current PID:");
    to_str(current_task ? current_task->pid : 0xFFFF, str); print(str);
    for (int i = 0; i < g_task_count; i++) {
        Task* t = &g_task_pool[i];
        print(t->name);
        print((char*)" PID:"); to_str(t->pid, str); print(str);
        print((char*)" "); print((char*)_sched_state_name(t->state));
    }
    print((char*)"──────────────────────────────────");
}

#endif // scheduler_h