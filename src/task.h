#ifndef task_h
#define task_h

// ── task.h ────────────────────────────────────────────────────────────────────
//
// Process Control Block (PCB) for the kernel task system.
//
// FIELD-ORDER CONTRACT
//   switch_context() in scheduler.h is a naked asm function that accesses two
//   fields by hard-coded byte displacements:
//
//       saved_rsp  @ byte offset  8
//       cr3        @ byte offset 16
//
//   The static_asserts below will fire at compile time if the layout ever
//   changes.  Update switch_context's asm to match before removing them.
//
// TASK LIFECYCLE
//
//   create_task()  →  Ready
//   schedule()     →  Ready → Running
//   yield()        →  Running → Ready  (re-enqueued)
//   block_task()   →  Running → Blocked (not re-enqueued)
//   unblock_task() →  Blocked → Ready  (re-enqueued)
//   task_exit()    →  Running → Dead   (never re-enqueued)
//
// ─────────────────────────────────────────────────────────────────────────────

#include <stdint.h>

// Default kernel-stack size per task (64 KiB is plenty for early work).
#define TASK_STACK_SIZE  (64ULL * 1024ULL)

// Maximum number of tasks in the static pool (excluding the idle task).
#define MAX_TASKS  16

// ── TaskState ─────────────────────────────────────────────────────────────────

enum class TaskState : uint8_t {
    Ready   = 0,   // in the ready queue, waiting for the CPU
    Running = 1,   // currently executing (== current_task)
    Blocked = 2,   // waiting for an event; not in the ready queue
    Dead    = 3,   // finished; stack can be reclaimed by a future reaper
};

// ── Task (PCB) ────────────────────────────────────────────────────────────────

struct Task {
    // ── ASM-critical fields — DO NOT REORDER (see static_asserts below) ──────
    uint32_t      pid;          // offset  0  (4 bytes)
    TaskState     state;        // offset  4  (1 byte)
    uint8_t       _pad[3];      // offset  5  (3 bytes natural alignment padding)
    uint64_t      saved_rsp;    // offset  8  ← switch_context saves/loads here
    uint64_t      cr3;          // offset 16  ← switch_context loads here
    //                                           (0 = share the kernel PML4)
    // ── General fields ────────────────────────────────────────────────────────
    uint8_t*      stack_base;   // base of the kmalloc'd kernel stack
    uint64_t      stack_size;   // size in bytes (always TASK_STACK_SIZE)
    void        (*entry)(uint64_t);  // entry function called on first run
    uint64_t      arg;          // argument forwarded to entry()
    Task*         next;         // intrusive singly-linked queue pointer
    char          name[32];     // human-readable name for debugger / dump
};

// ── Layout guards ─────────────────────────────────────────────────────────────
// If either assert fires, update the hard-coded offsets in switch_context.

static_assert(__builtin_offsetof(Task, saved_rsp) ==  8,
    "Task::saved_rsp offset changed — update switch_context asm (8(%rdi/rsi))");
static_assert(__builtin_offsetof(Task, cr3)        == 16,
    "Task::cr3 offset changed — update switch_context asm (16(%rsi))");

#endif // task_hs