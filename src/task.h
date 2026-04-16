#ifndef task_h
#define task_h

// ── task.h ────────────────────────────────────────────────────────────────────
//
// Phase 3 — Task Model (PCB)
//
// A Task is the scheduler's unit of execution.  Every kernel-mode task gets:
//   • A unique PID.
//   • A private kernel stack (allocated from the heap).
//   • A saved RSP — the only CPU state the context switcher needs to restore
//     callee-saved registers and resume execution.
//   • A CR3 slot — physical address of the task's PML4.  0 = inherit current
//     address space (all kernel tasks share the boot PML4).
//   • A state field used by the scheduler (READY / RUNNING / BLOCKED / DEAD).
//   • An intrusive `next` pointer for the scheduler's circular run-queue.
//
// USAGE
//   Task* t = task_create(my_entry_fn);
//   scheduler_add_task(t);          // defined in scheduler.h
//
// Tasks should not return from their entry function.  Call task_exit() (in
// scheduler.h) to cleanly remove the task from the run-queue.
//
// DEPENDENCIES
//   heap.h        — kmalloc / kfree
//   pagingstuff.h — PAGE_SIZE
//   helpers.h     — print, hcf
// ──────────────────────────────────────────────────────────────────────────────

#include <stdint.h>
#include "pagingstuff.h"
#include "heap.h"
#include "helpers.h"
#include "vfs.h"

#define MAX_FDS 16

// ── TaskState ──────────────────────────────────────────────────────────────────

enum class TaskState : uint8_t {
    READY   = 0,   // on the run-queue, waiting for CPU time
    RUNNING = 1,   // currently executing
    BLOCKED = 2,   // waiting for an event; removed from consideration by schedule()
    DEAD    = 3,   // exited; will be freed on next schedule() pass
};

// ── PID type ───────────────────────────────────────────────────────────────────

using pid_t = uint32_t;

// ── Default kernel stack (per task) ───────────────────────────────────────────
// 4 pages = 16 KiB.  Enough for several levels of nested kernel calls.

#define TASK_DEFAULT_STACK_PAGES  4u

// ── PCB ────────────────────────────────────────────────────────────────────────
// Keep this struct small — it lives in the kernel heap and is iterated every
// scheduler tick.

struct Task {
    // ── Identity ──────────────────────────────────────────────────────────────
    pid_t     pid;
    TaskState state;
    bool      yielded;     // true if task voluntarily called yield()
    uint8_t   _pad[2];

    // ── Saved CPU context ─────────────────────────────────────────────────────
    // After a context switch OUT, the stack contains (from high to low):
    //   [return address / entry point]
    //   [rbp] [rbx] [r12] [r13] [r14] [r15]   ← saved_rsp points here
    //
    // switch_context() in scheduler.h mirrors this layout exactly.
    uint64_t saved_rsp;

    // ── Virtual memory ────────────────────────────────────────────────────────
    // Physical address of this task's PML4.
    // 0 = no CR3 switch on context swap (task shares the current address space).
    // For user tasks: set this to the value returned by create_user_page_table().
    uint64_t cr3;

    // ── Kernel stack ──────────────────────────────────────────────────────────
    uint8_t* stack_base;   // lowest byte of the allocation (stack grows DOWN)
    uint64_t stack_size;   // total bytes; 0 for the idle/boot task

    // ── Scheduler intrusive list ──────────────────────────────────────────────
    // Tasks are held in a circular singly-linked list so the scheduler can
    // walk the run-queue in O(1) without a heap allocation per switch.
    Task* next;
    
    // ── File Descriptors ──────────────────────────────────────────────────────
    file* fd_table[MAX_FDS];
    
    // ── Userspace Context ─────────────────────────────────────────────────────
    uint64_t user_entry;
    uint64_t user_stack;

    // ── Sleep / Block state ───────────────────────────────────────────────────
    uint64_t wake_at_tick;  // Tick count when this task should be unblocked; 0 = manual unblock
};

// ── Global PID counter ─────────────────────────────────────────────────────────

static pid_t g_next_pid = 0;

// ── task_alloc ────────────────────────────────────────────────────────────────
// Allocate and zero a Task from the kernel heap.
// Internal helper — callers use task_create() instead.

static Task* task_alloc() {
    Task* t = (Task*)kmalloc(sizeof(Task));
    if (!t) { print((char*)"[task] OOM allocating PCB"); hcf(); }

    uint8_t* raw = (uint8_t*)t;
    for (uint64_t i = 0; i < sizeof(Task); i++) raw[i] = 0;

    t->pid   = g_next_pid++;
    t->state = TaskState::READY;
    for (int i = 0; i < MAX_FDS; i++) {
        t->fd_table[i] = nullptr;
    }
    
    return t;
}

// ── task_alloc_fd ─────────────────────────────────────────────────────────────
// Find the lowest available file descriptor and assign it. Returns FD, or -1.

static int task_alloc_fd(Task* t, file* f) {
    // FDs 0, 1, 2 are reserved for TTY (stdin, stdout, stderr)
    for (int i = 3; i < MAX_FDS; i++) {
        if (t->fd_table[i] == nullptr) {
            t->fd_table[i] = f;
            return i;
        }
    }
    return -1;
}

// ── task_setup_stack ──────────────────────────────────────────────────────────
// Write an initial stack frame for `t` so that the very first call to
// switch_context() targeting this task works correctly.
//
// Layout built from the TOP of the stack downwards:
//
//   top - 8   : sentinel 0   (crash pad if entry() ever returns — it shouldn't)
//   top - 16  : entry        ← switch_context's final `ret` pops this as RIP
//   top - 24  : 0            ← rbp
//   top - 32  : 0            ← rbx
//   top - 40  : 0            ← r12
//   top - 48  : 0            ← r13
//   top - 56  : 0            ← r14
//   top - 64  : 0            ← r15   ← t->saved_rsp is set here
//
// After switch_context pops r15..rbp (6 × 8 = 48 bytes) and executes `ret`:
//   RSP = top - 16 + 48 = top - 16 + 48 ... wait let me recalculate.
//
//   saved_rsp = top - 64
//   pop r15: rsp = top - 56
//   pop r14: rsp = top - 48
//   pop r13: rsp = top - 40
//   pop r12: rsp = top - 32
//   pop rbx: rsp = top - 24
//   pop rbp: rsp = top - 16
//   ret:     rsp = top - 8   ← pops `entry` into RIP
//
// At function entry RSP = top - 8.  `top` is a kmalloc end address which is
// always HEAP_ALIGN (16-byte) aligned, so (top - 8) % 16 == 8 — exactly the
// System V AMD64 ABI requirement (RSP % 16 == 8 at function entry after CALL).

static void task_setup_stack(Task* t, void (*entry)()) {
    uint64_t* sp = (uint64_t*)(t->stack_base + t->stack_size);

    *--sp = 0ULL;             // sentinel — crash if entry() returns
    *--sp = (uint64_t)entry;  // "return address" for switch_context's ret
    *--sp = 0ULL;             // rbp
    *--sp = 0ULL;             // rbx
    *--sp = 0ULL;             // r12
    *--sp = 0ULL;             // r13
    *--sp = 0ULL;             // r14
    *--sp = 0ULL;             // r15   ← initial saved_rsp

    t->saved_rsp = (uint64_t)sp;
}

// ── task_create ───────────────────────────────────────────────────────────────
// Allocate a new kernel-mode task that will begin executing at `entry`.
// Returns the Task pointer.  The task is in READY state but NOT yet added to
// the scheduler's run-queue — call scheduler_add_task(t) for that.
//
// `stack_pages` — number of 4 KiB pages for the stack (0 = use default 4).

static Task* task_create(void (*entry)(), uint32_t stack_pages = 0) {
    if (stack_pages == 0) stack_pages = TASK_DEFAULT_STACK_PAGES;

    Task* t = task_alloc();

    t->stack_size = (uint64_t)stack_pages * PAGE_SIZE;
    t->stack_base = (uint8_t*)kmalloc(t->stack_size);
    if (!t->stack_base) { print((char*)"[task] OOM allocating stack"); hcf(); }

    // Zero the stack so uninitialised reads give predictable results.
    for (uint64_t i = 0; i < t->stack_size; i++) t->stack_base[i] = 0;

    task_setup_stack(t, entry);

    char str[32];
    print((char*)"[task] created PID:");
    to_str(t->pid, str); print(str);

    return t;
}

// ── task_create_user ──────────────────────────────────────────────────────────
// Like task_create but binds a dedicated page table (from create_user_page_table).
// Useful for future user-mode processes — the scheduler will switch CR3 when
// scheduling this task.

static Task* task_create_user(void (*entry)(), uint64_t cr3_phys,
                               uint32_t stack_pages = 0) {
    Task* t = task_create(entry, stack_pages);
    t->cr3  = cr3_phys;
    return t;
}

// ── task_free ─────────────────────────────────────────────────────────────────
// Release all resources owned by a Task.
// Only call this AFTER the task has been removed from the scheduler's queue.

static void task_free(Task* t) {
    if (!t) return;
    
    for (int i = 0; i < MAX_FDS; i++) {
        if (t->fd_table[i] != nullptr) {
            vfs_close(t->fd_table[i]);
            kfree(t->fd_table[i]);
        }
    }

    if (t->stack_size > 0 && t->stack_base)
        kfree(t->stack_base);
    kfree(t);
}

#endif // task_h