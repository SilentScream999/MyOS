#ifndef scheduler_h
#define scheduler_h

// ── scheduler.h ───────────────────────────────────────────────────────────────
//
// Phase 3 — Round-Robin Scheduler + Context Switcher
//
// OVERVIEW
//   Tasks live in a circular singly-linked list (the "run-queue").
//   schedule() walks the list to find the next READY task and calls
//   switch_context() to swap execution onto it.
//
//   Timer integration
//   -----------------
//   Call scheduler_tick() from your timer IRQ handler every tick.
//   At 1000 Hz with SCHEDULER_TICKS = 10 you get 10 ms time slices.
//
//   Example — in your timer IRQ handler or timer.h callback:
//
//       #include "scheduler.h"
//       // ... inside the IRQ handler:
//       scheduler_tick();
//
//   That is the ONLY change needed in existing code.
//
// INITIALISATION ORDER (in kmain)
//   init_heap();
//   scheduler_init();          <- wraps kmain itself as the idle task (PID 0)
//   Task* t = task_create(my_fn);
//   scheduler_add_task(t);
//   // ... existing code continues; tasks run during timer ticks
//
// CONTEXT SWITCH MECHANISM
//   switch_context() is a naked function (no compiler-generated prologue).
//   It saves rbp, rbx, r12-r15 onto the OLD task's stack and RSP into
//   old->saved_rsp, then loads new->saved_rsp, pops those registers, and
//   returns into the new task's saved instruction pointer.
//   See task_setup_stack() in task.h for the matching initial stack layout.
//
// DEPENDENCIES
//   task.h        -- Task, TaskState, task_free
//   helpers.h     -- print, hcf
// ──────────────────────────────────────────────────────────────────────────────

#include "task.h"
#include "gdt.h"
#include "helpers.h"

// ── Scheduler configuration ───────────────────────────────────────────────────

// Timer ticks per time slice.  At 1000 Hz: 10 ticks = 10 ms per task.
#define SCHEDULER_TICKS  10u

// ── Module state ───────────────────────────────────────────────────────────────

Task*    g_current_task    = nullptr;
Task*    g_task_list       = nullptr;   // head of circular run-queue
bool     g_scheduler_ready = false;
static uint64_t g_kernel_cr3      = 0;         // master kernel PML4 physical address
static uint32_t g_tick_counter    = 0;         // added back for time-slice tracking
static pid_t g_terminal_shell_pid = 0;
static pid_t g_terminal_foreground_pid = 0;
static void (*g_task_exit_hook)(pid_t) = nullptr;

// Mark only user processes for interruption. Do not free resources from a key
// IRQ or abandon a suspended kernel operation; cancellation happens at a safe
// syscall boundary or when the task is suspended in user mode.
static bool task_request_interrupt(pid_t pid) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    bool found = false;
    Task* t = g_task_list;
    if (t && pid && pid != g_terminal_shell_pid) do {
        if (t->pid == pid && t->user_entry && t->state != TaskState::DEAD) {
            t->interrupt_requested = true;
            if (t->state == TaskState::BLOCKED) { t->wake_at_tick = 0; t->state = TaskState::READY; }
            found = true; break;
        }
        t = t->next;
    } while (t != g_task_list);
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
    return found;
}
static void task_complete(Task* exiting, uint64_t code);

// CPU idle cycles end before interrupt work or scheduler preemption.
extern volatile uint64_t g_idle_tsc_accum;
#include "cpu_idle.h"
static CpuIdleClock g_cpu_idle_clock{};
static void scheduler_idle_interrupt() {
    uint64_t now;
    __asm__ volatile("rdtsc; shl $32, %%rdx; or %%rdx, %%rax" : "=a"(now) :: "rdx");
    g_idle_tsc_accum=g_idle_tsc_accum+g_cpu_idle_clock.end(now);
}
// Caller holds IRQs disabled. Interrupt entry closes the interval even if the
// handler schedules another task before this suspended stack can resume.
static void scheduler_idle_halt() {
    uint64_t now;
    __asm__ volatile("rdtsc; shl $32, %%rdx; or %%rdx, %%rax" : "=a"(now) :: "rdx");
    g_cpu_idle_clock.begin(now);
    __asm__ volatile("sti; hlt; cli" ::: "memory");
    scheduler_idle_interrupt();
}

// Forward declaration from timer.h (cannot include timer.h here due to circular deps)
extern volatile uint64_t g_tick_count;

// ── switch_context ────────────────────────────────────────────────────────────
// Low-level kernel context switch.
//
// void switch_context(uint64_t* old_rsp_out, uint64_t new_rsp, uint64_t new_cr3)
//
//   old_rsp_out (rdi) -- pointer into the old Task PCB; receives its saved RSP.
//   new_rsp     (rsi) -- saved RSP of the task we are switching TO.
//   new_cr3     (rdx) -- physical PML4 address for the new task; 0 = no switch.
//
// The naked attribute suppresses the compiler's own prologue/epilogue so the
// function body is exactly what we write in the asm block.
//
// Callee-saved registers (SysV AMD64 ABI): rbp, rbx, r12, r13, r14, r15.
// Saving these is sufficient to make the old task resumable.

__attribute__((naked))
static void switch_context(uint64_t* old_rsp_out, uint64_t new_rsp, uint64_t new_cr3) {
    asm(
        // Save old task's callee-saved registers onto its kernel stack.
        "push %rbp          \n"
        "push %rbx          \n"
        "push %r12          \n"
        "push %r13          \n"
        "push %r14          \n"
        "push %r15          \n"

        // Save old task's RSP into Task::saved_rsp (rdi = old_rsp_out).
        "mov  %rsp, (%rdi)  \n"

        // Load new task's RSP (rsi = new_rsp).
        "mov  %rsi, %rsp    \n"

        // Switch CR3 only when non-zero (different address space).
        // Writing CR3 flushes the entire TLB; skip to save ~150 cycles when
        // both tasks share the same address space.
        "test %rdx, %rdx    \n"
        "jz   1f            \n"
        "mov  %rdx, %cr3    \n"
        "1:                 \n"

        // Restore new task's callee-saved registers.
        // For a fresh task, task_setup_stack() put zeroes here.
        // For a previously-preempted task, these are its real saved values.
        "pop  %r15          \n"
        "pop  %r14          \n"
        "pop  %r13          \n"
        "pop  %r12          \n"
        "pop  %rbx          \n"
        "pop  %rbp          \n"

        // ret pops the top of the new stack as RIP:
        //   fresh task  -> its entry function
        //   resumed task -> return address inside a previous switch_context call
        "ret                \n"
    );
}

// ── scheduler_add_task ────────────────────────────────────────────────────────
// Insert a task into the circular run-queue and mark it READY.

static void scheduler_add_task(Task* t) {
    if (!t) return;
    
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");

    t->state = TaskState::READY;

    if (!g_task_list) {
        g_task_list = t;
        t->next     = t;
    } else {
        // Insert right after the head so the new task runs soon.
        t->next           = g_task_list->next;
        g_task_list->next = t;
    }

    __asm__ volatile("push %0; popfq" :: "r"(rflags) : "memory", "cc");
}

// ── scheduler_reap_dead ───────────────────────────────────────────────────────
// Walk the run-queue and free any DEAD tasks.
// Called lazily from schedule() — no dedicated reaper thread needed.

static void scheduler_reap_dead() {
    if (!g_task_list || g_task_list->next == g_task_list) return;

    Task* prev = g_task_list;
    Task* cur  = g_task_list->next;

    while (cur != g_task_list) {
        Task* nxt = cur->next;
        if (cur != g_current_task && cur->state == TaskState::DEAD &&
            (cur->parent_pid == 0 || cur->exit_collected)) {
            prev->next = nxt;
            task_free(cur);
        } else {
            prev = cur;
        }
        cur = nxt;
    }

    // Check the head node itself (never remove if it is the current task).
    if (g_task_list != g_current_task &&
        g_task_list->state == TaskState::DEAD &&
        (g_task_list->parent_pid == 0 || g_task_list->exit_collected) &&
        g_task_list->next  != g_task_list) {
        Task* old_head = g_task_list;
        prev->next  = old_head->next;
        g_task_list = old_head->next;
        task_free(old_head);
    }
}

static void schedule() {
    if (!g_scheduler_ready || !g_task_list) return;

    // Prevent re-entrancy: save interrupt state and disable interrupts
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");

    scheduler_reap_dead();

    Task* pending = g_task_list;
    do {
        if (pending != g_current_task && pending->state != TaskState::DEAD &&
            pending->interrupt_requested && !pending->in_syscall)
            task_complete(pending, 130);
        pending = pending->next;
    } while (pending != g_task_list);

    // Check for any tasks that need to be unblocked
    Task* scan = g_task_list;
    do {
        if (scan->state == TaskState::BLOCKED && scan->wake_at_tick > 0) {
            if (g_tick_count >= scan->wake_at_tick) {
                scan->state = TaskState::READY;
                scan->wake_at_tick = 0;
            }
        }
        scan = scan->next;
    } while (scan != g_task_list);

    // Search starting from the task AFTER the current one.
    Task* start = g_current_task ? g_current_task->next : g_task_list;
    Task* next  = start;
    uint32_t guard = 0;

    // --- PRIORITY BOOST FOR COMPOSITOR (PID 1) ---
    // If the compositor is ready, it should almost always run immediately to maintain 60FPS.
    Task* comp = g_task_list;
    do {
        if (comp->pid == 1 && comp->state == TaskState::READY) {
            next = comp;
            goto switch_now;
        }
        comp = comp->next;
    } while (comp != g_task_list);

    do {
        if (next->state == TaskState::READY) break;
        next = next->next;
        if (++guard > 1024) {
            // list corruption
            __asm__ volatile("push %0; popfq" :: "r"(rflags) : "memory", "cc");
            return;
        }
    } while (next != start);

switch_now:

    // Nothing to switch to.
    if (next == g_current_task || next->state != TaskState::READY) {
        if (g_current_task && g_current_task->yielded) {
             g_current_task->yielded = false;
             // Voluntary yield with no one else ready: idle until next interrupt.
             scheduler_idle_halt();
        }
        __asm__ volatile("push %0; popfq" :: "r"(rflags) : "memory", "cc");
        return;
    }

    Task* old = g_current_task;

    if (old && old->state == TaskState::RUNNING)
        old->state = TaskState::READY;
    next->state    = TaskState::RUNNING;
    next->yielded  = false; // always clear yielded flag when starting a turn
    g_current_task = next;

    // Update TSS.rsp[0] and g_current_kstack_top so hardware interrupts and syscalls
    // from Ring 3 land on this task's private kernel stack.
    if (next->stack_size > 0 && next->stack_base) {
        g_current_kstack_top = (uint64_t)(next->stack_base + next->stack_size);
    }
    g_tss.rsp[0] = g_current_kstack_top;

    switch_context(&old->saved_rsp, next->saved_rsp, next->cr3);
    
    // Execution resumes here when the old task is scheduled back in.
    // Restore the exactly matching interrupt state we had before yielding.
    __asm__ volatile("push %0; popfq" :: "r"(rflags) : "memory", "cc");
}

// ── yield ─────────────────────────────────────────────────────────────────────
// Voluntarily surrender the rest of the current time slice.

static void yield() {
    if (g_current_task) g_current_task->yielded = true;
    schedule();
}

// ── task_sleep_ms ─────────────────────────────────────────────────────────────
// Block the current task for at least `ms` milliseconds.
// Yields the CPU immediately.

static void task_sleep_ms(uint64_t ms) {
    if (!g_current_task || !ms) return;
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    Task* self = g_current_task;
    self->wake_at_tick = g_tick_count + ms;
    self->state = TaskState::BLOCKED;
    while (self->state == TaskState::BLOCKED) {
        schedule();
        if (self->state == TaskState::BLOCKED) {
            scheduler_idle_halt();
        }
    }
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

// Wait for one child, retaining completed children until status is collected.

static int64_t task_wait_child(pid_t pid, uint64_t* exit_code) {
    if (!g_current_task || !pid || pid == g_current_task->pid) return -1;
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    Task* parent = g_current_task;
    for (;;) {
        if (parent->interrupt_requested) {
            parent->wait_pid = 0;
            __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
            return -1;
        }
        Task* child = nullptr;
        Task* scan = g_task_list;
        if (scan) do {
            if (scan->pid == pid && scan->parent_pid == parent->pid && !scan->exit_collected) { child = scan; break; }
            scan = scan->next;
        } while (scan != g_task_list);
        if (!child) {
            __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
            return -1;
        }
        if (child->state == TaskState::DEAD) {
            if (exit_code) *exit_code = child->exit_code;
            child->exit_collected = true;
            parent->wait_pid = 0;
            __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
            return pid;
        }
        // Interrupts remain disabled between testing exit and blocking, so an
        // exit cannot race the waiter into sleeping after its wakeup was sent.
        parent->wait_pid = pid;
        parent->wake_at_tick = 0;
        parent->state = TaskState::BLOCKED;
        schedule();
        if (parent->state == TaskState::BLOCKED)
            scheduler_idle_halt();
    }
}

static void task_complete(Task* exiting, uint64_t code) {
    if (exiting && exiting->state != TaskState::DEAD) {
        if (g_task_exit_hook) g_task_exit_hook(exiting->pid);
        if (g_terminal_foreground_pid == exiting->pid) g_terminal_foreground_pid = 0;
        exiting->exit_code = code;
        // Release descriptors now, even if the parent has not waited yet.
        for (int fd = 0; fd < MAX_FDS; ++fd) if (exiting->fd_table[fd]) {
            vfs_close(exiting->fd_table[fd]);
            kfree(exiting->fd_table[fd]);
            exiting->fd_table[fd] = nullptr;
        }
        Task* scan = g_task_list;
        if (scan) do {
            if (scan->parent_pid == exiting->pid) scan->parent_pid = 0;
            if (scan->pid == exiting->parent_pid && scan->wait_pid == exiting->pid && scan->state == TaskState::BLOCKED) {
                scan->wait_pid = 0;
                scan->state = TaskState::READY;
            }
            scan = scan->next;
        } while (scan != g_task_list);
        exiting->state = TaskState::DEAD;
    }
}

[[noreturn]] static void task_exit(uint64_t code = 0) {
    __asm__ volatile("cli" ::: "memory");
    task_complete(g_current_task, code);
    // The parent may still be asleep (exit-before-wait), and the compositor
    // and input task can both be sleeping too. With no READY task, schedule()
    // returns. Keep this dead task's stack alive and idle until a timer wakes
    // another task instead of halting the whole OS.
    for (;;) {
        schedule();
        scheduler_idle_halt();
    }
}

// ── scheduler_tick ────────────────────────────────────────────────────────────
// Call from your timer IRQ handler every tick (1000 Hz).
// Triggers schedule() every SCHEDULER_TICKS ticks = 10 ms slices.
//
// HOW TO WIRE IT UP
// -----------------
// Find your timer IRQ handler (probably in timer.h or irq.h) and add
// one line at the end of the handler body:
//
//     scheduler_tick();
//
// That is the only change needed in existing code.

static void scheduler_tick() {
    if (!g_scheduler_ready) return;
    if (++g_tick_counter >= SCHEDULER_TICKS) {
        g_tick_counter = 0;
        schedule();
    }
}

// ── scheduler_init ────────────────────────────────────────────────────────────
// Bootstrap the scheduler.  Wraps the currently-running context (kmain) as
// the idle task (PID 0).  The idle task keeps executing its existing loop
// whenever all other tasks are blocked or dead.
//
// Call once, after init_heap() and before adding other tasks.

static void scheduler_init() {
    Task* idle = task_alloc();
    idle->state      = TaskState::RUNNING;
    idle->stack_base = nullptr;   // boot stack -- not heap-allocated, never freed
    idle->stack_size = 0;
    // VERY IMPORTANT: Record the authoritative kernel CR3 so returning to kmain 
    // from a user task flushes the TLB back to the master page tables.
    idle->cr3        = read_cr3() & PTE_ADDR_MASK;
    // Save the master kernel CR3 so syscall_entry can switch back to it
    g_kernel_cr3     = idle->cr3;
    // idle->saved_rsp is written on the first switch_context call away from here.

    g_current_task = idle;
    g_task_list    = idle;
    idle->next     = idle;

    g_scheduler_ready = true;

    char str[32];
    print((char*)"[sched] ready. Idle PID:");
    to_str(idle->pid, str); print(str);
}

// ── scheduler_status ──────────────────────────────────────────────────────────
// Debug helper: dump all tasks in the run-queue.

static void scheduler_status() {
    char str[32];
    print((char*)"-- Scheduler status --");
    Task* t = g_task_list;
    if (!t) { print((char*)"  (empty)"); return; }
    do {
        print((char*)"PID:"); to_str(t->pid, str); print(str);
        const char* st =
            (t->state == TaskState::READY)   ? "READY"   :
            (t->state == TaskState::RUNNING)  ? "RUNNING" :
            (t->state == TaskState::BLOCKED)  ? "BLOCKED" : "DEAD";
        print((char*)st);
        if (t == g_current_task) print((char*)"<-- current");
        t = t->next;
    } while (t != g_task_list);
}

#endif // scheduler_h
