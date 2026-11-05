#ifndef syscall_h
#define syscall_h

// ── syscall.h ─────────────────────────────────────────────────────────────────
//
// Phase 4 — Syscall Interface + User Mode Prep
//
// MECHANISM: SYSCALL / SYSRET  (AMD64 fast-call path, same as Linux x86-64)
//
//   SYSCALL is a non-privileged instruction.  When executed it:
//     1. Saves RIP    → RCX   (return address for SYSRET)
//     2. Saves RFLAGS → R11   (restored by SYSRET)
//     3. Loads RIP from IA32_LSTAR MSR   (our entry point)
//     4. Loads CS/SS from IA32_STAR MSR  (kernel selectors)
//     5. Clears RFLAGS bits set in IA32_SFMASK (disables interrupts)
//
//   SYSRET (REX.W=1, 64-bit):
//     1. Restores RIP    from RCX
//     2. Restores RFLAGS from R11
//     3. CS = STAR[63:48]+16 | 3,  SS = STAR[63:48]+8 | 3
//
// MSR VALUES (derived from gdt.h layout):
//   STAR[47:32] = 0x08  syscall  CS=0x08(kcode)  SS=0x08+8=0x10(kdata)
//   STAR[63:48] = 0x10  sysretq  CS=0x10+16=0x20|3  SS=0x10+8=0x18|3
//                                   SEG_USER_CODE         SEG_USER_DATA
//   LSTAR       = &syscall_entry
//   SFMASK      = (1<<9)   clear IF on syscall entry
//   EFER       |= (1<<0)   SCE — enables the SYSCALL instruction
//
// CALLING CONVENTION (Linux x86-64 ABI):
//   RAX = syscall number (in) / return value (out)
//   RDI = arg1,  RSI = arg2,  RDX = arg3
//   RCX destroyed by SYSCALL (holds user RIP)
//   R11 destroyed by SYSCALL (holds user RFLAGS)
//
// KERNEL STACK:
//   A dedicated 32 KiB static stack is used for every syscall.
//   TSS.rsp[0] is also pointed here so hardware interrupts arriving in
//   ring 3 land on a valid stack.
//
// RING 3 TRANSITION:
//   enter_usermode(entry_va, user_stack_va) builds an IRETQ frame and
//   executes it, landing at entry_va in ring 3 with RSP = user_stack_va.
//
// SYSCALL NUMBERS (match Linux x86-64 for forward compatibility):
//    1 = sys_write   (fd, buf_va, len)  -> bytes written
//   39 = sys_getpid ()                  -> current PID
//   60 = sys_exit   (code)              -> does not return
//
// DEPENDENCIES:
//   gdt.h       -- g_tss, SEG_USER_CODE, SEG_USER_DATA
//   scheduler.h -- g_current_task, task_exit()
//   helpers.h   -- print, hcf
// ──────────────────────────────────────────────────────────────────────────────

#include <stdint.h>
#include "gdt.h"
#include "helpers.h"
#include "scheduler.h"
#include "vfs.h"
#include "tty.h"
#include "acpi.h"
#include "wm.h"

// ── MSR addresses ─────────────────────────────────────────────────────────────

#define MSR_EFER   0xC0000080u
#define MSR_STAR   0xC0000081u
#define MSR_LSTAR  0xC0000082u
#define MSR_SFMASK 0xC0000084u

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)(val);
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"(lo), "d"(hi) : "memory");
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

// ── Syscall kernel stack tracking ─────────────────────────────────────────────
// Every user task has its own private kernel stack allocated in its Task PCB.
// g_current_kstack_top is maintained by the scheduler so syscall_entry switches
// directly to the current task's private kernel stack.
// g_user_rsp_scratch is a temporary stash used ONLY while interrupts are disabled
// before user RSP is pushed onto the task's private stack.


// ── Syscall numbers ───────────────────────────────────────────────────────────

#define SYS_READ    0u
#define SYS_WRITE   1u
#define SYS_OPEN    2u
#define SYS_CLOSE   3u
#define SYS_SCROLL  16u
#define SYS_TTYRAW  17u
#define SYS_SLEEP   35u
#define SYS_GETPID  39u
#define SYS_READDIR 40u
#define SYS_SPAWN   59u
#define SYS_EXIT    60u
#define SYS_SHUTDOWN 88u
#define SYS_REBOOT   169u

#define SYS_WIN_CREATE    200u
#define SYS_WIN_SYNC      201u
#define SYS_WIN_DESTROY   202u
#define SYS_WIN_GET_BUFFER 203u
#define SYS_WIN_DAMAGE     204u   // (win_id, x, y, w, h): part of the surface changed

// ── string copying from user ──────────────────────────────────────────────────
// For safety we should check if buf_va is within user limits.
// Right now kernel/user space is unified in our prototype PML4.
static void copy_string_from_user(char* dest, uint64_t src_va, uint64_t max_len) {
    const char* src = (const char*)src_va;
    uint64_t i = 0;
    while (i < max_len - 1 && src[i] != '\0') {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

static void copy_string_to_user(uint64_t dest_va, const char* src, uint64_t len) {
    char* dest = (char*)dest_va;
    for (uint64_t i = 0; i < len; i++) {
        dest[i] = src[i];
    }
}

// ── sys_open ──────────────────────────────────────────────────────────────────

static uint64_t sys_open(uint64_t path_va, uint64_t flags) {
    if (!g_current_task) return (uint64_t)-1;
    
    char path[256];
    copy_string_from_user(path, path_va, sizeof(path));
    
    file* f = (file*)kmalloc(sizeof(file));
    if (!f) return (uint64_t)-1;
    
    if (vfs_open(path, flags, f) == 0) {
        int fd = task_alloc_fd(g_current_task, f);
        if (fd != -1) return fd;
        // FD table is full
        vfs_close(f);
    }
    kfree(f);
    return (uint64_t)-1;
}

// ── sys_close ─────────────────────────────────────────────────────────────────

static uint64_t sys_close(uint64_t fd) {
    if (!g_current_task || fd >= MAX_FDS) return (uint64_t)-1;
    if (fd <= 2) return 0; // standard FDs, ignore for now
    
    file* f = g_current_task->fd_table[fd];
    if (f) {
        vfs_close(f);
        kfree(f);
        g_current_task->fd_table[fd] = nullptr;
        return 0;
    }
    return (uint64_t)-1;
}

// ── sys_read ──────────────────────────────────────────────────────────────────

static uint64_t sys_read(uint64_t fd, uint64_t buf_va, uint64_t len) {
    if (!g_current_task) return (uint64_t)-1;
    if (len == 0) return 0;
    
    if (fd == 0) { // stdin
        char tmp[128];
        size_t to_read = len > 128 ? 128 : len;
        size_t read_bytes = tty_read(tmp, to_read);
        copy_string_to_user(buf_va, tmp, read_bytes);
        return read_bytes;
    }
    
    if (fd >= MAX_FDS) return (uint64_t)-1;
    
    file* f = g_current_task->fd_table[fd];
    if (f) {
        return vfs_read(f, (void*)buf_va, len);
    }
    return (uint64_t)-1;
}

// ── sys_write ─────────────────────────────────────────────────────────────────
// fd 1 (stdout) and fd 2 (stderr) print to the terminal emulator.

static uint64_t sys_write(uint64_t fd, uint64_t buf_va, uint64_t len) {
    if (!g_current_task) return (uint64_t)-1;
    if (len == 0) return 0;
    
    if (fd == 1 || fd == 2) {
        char* tmp = (char*)kmalloc(len + 1);
        if (!tmp) return (uint64_t)-1;
        
        copy_string_from_user(tmp, buf_va, len + 1); // +1 because it reserves the last byte for '\0'
        tty_write(tmp, len); // tty_write doesn't care about the null terminator, it uses length
        kfree(tmp);
        return len;
    }
    
    if (fd < MAX_FDS && g_current_task->fd_table[fd]) {
        return vfs_write(g_current_task->fd_table[fd], (const void*)buf_va, len);
    }
    
    return (uint64_t)-1;
}

// ── sys_getpid ────────────────────────────────────────────────────────────────

static uint64_t sys_getpid() {
    return g_current_task ? (uint64_t)g_current_task->pid : 0ULL;
}

// ── sys_exit ──────────────────────────────────────────────────────────────────

[[noreturn]] static void sys_exit(uint64_t /*code*/) {
    if (g_current_task) task_exit();   // marks DEAD + schedule() — no return
    hcf();
}

// ── sys_shutdown ──────────────────────────────────────────────────────────────

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static uint64_t sys_shutdown() {
    // Real Hardware ACPI poweroff (prefers UEFI ResetSystem, falls back to raw AML parsing)
    hardware_shutdown();

    // QEMU / Bochs ACPI poweroff
    outw(0x604, 0x2000);
    
    // VirtualBox poweroff
    outw(0x4004, 0x3400);

    hcf(); // fallback
    return 0;
}

static uint64_t sys_scroll(int64_t delta) {
    term_scroll_view(delta);
    return 0;
}

static uint64_t sys_ttyraw(uint64_t raw) {
    tty_set_raw(raw != 0);
    return 0;
}

// ── sys_win_create ────────────────────────────────────────────────────────────
static uint64_t sys_win_create(uint64_t title_va, uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint64_t flags) {
    char title[64];
    copy_string_from_user(title, title_va, sizeof(title));
    bool closable = (flags & 1);
    Window* win = wm_create_window(title, (int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, closable);
    if (!win) return 0;
    return (uint64_t)win->id;
}

// ── sys_win_sync ──────────────────────────────────────────────────────────────
// Whole surface changed. (Before: only set g_needs_refresh, with NO dirty rect, so the compositor was never
// told WHICH part of the screen to recompose.)
static uint64_t sys_win_sync(uint64_t win_id) {
    Window* win = wm_find_window((int)win_id);
    if (win) wm_surface_damage_all(win);
    g_needs_refresh = true;
    return 0;
}

// ── sys_win_damage ────────────────────────────────────────────────────────────
// Partial update: only the given rectangle of the surface (surface coordinates) is recomposed.
static uint64_t sys_win_damage(uint64_t win_id, uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    if (x > 0x7FFFFFFFu || y > 0x7FFFFFFFu || w > 0x7FFFFFFFu || h > 0x7FFFFFFFu) return (uint64_t)-1;
    Window* win = wm_find_window((int)win_id);
    if (!win) return (uint64_t)-1;
    wm_surface_damage(win, (int32_t)x, (int32_t)y,
                      (int32_t)((x + w > 0x7FFFFFFFu) ? 0x7FFFFFFFu : x + w),
                      (int32_t)((y + h > 0x7FFFFFFFu) ? 0x7FFFFFFFu : y + h));
    return 0;
}

// ── sys_win_destroy ───────────────────────────────────────────────────────────
static uint64_t sys_win_destroy(uint64_t win_id) {
    wm_destroy_window((int)win_id);
    return 0;
}

// ── sys_win_get_buffer ────────────────────────────────────────────────────────
static uint64_t sys_win_get_buffer(uint64_t win_id) {
    for (int i = 0; i < g_window_count; i++) {
        if (g_window_list[i] && g_window_list[i]->id == (int)win_id) {
            return (uint64_t)g_window_list[i]->buffer;
        }
    }
    return 0;
}

// ── syscall_dispatch ──────────────────────────────────────────────────────────
// Called from the naked entry stub.  Returns the value to place in RAX.

static uint64_t sys_readdir(uint64_t path_va, uint64_t index, uint64_t name_out_va, uint64_t size_out_va, uint64_t type_out_va) {
    char path[256];
    copy_string_from_user(path, path_va, sizeof(path));
    char name[256];
    uint64_t sz = 0;
    VnodeType type = VnodeType::FILE;
    int res = vfs_readdir(path, index, name, &sz, &type);
    if (res <= 0) return (uint64_t)res; // 0 = EOF, -1 = error

    if (name_out_va) {
        char* u_name = (char*)name_out_va;
        int i = 0;
        while (name[i] && i < 255) { u_name[i] = name[i]; i++; }
        u_name[i] = '\0';
    }

    if (size_out_va) *(uint64_t*)size_out_va = sz;
    if (type_out_va) *(uint64_t*)type_out_va = (type == VnodeType::DIRECTORY) ? 2 : 1;
    return 1;
}

static Task* execve_path(const char* path);

// ── sys_sleep ─────────────────────────────────────────────────────────────────
// Block the calling task for `ms` milliseconds (rounded up to tick granularity).
// At 1000 Hz each tick = 1 ms, so ms == ticks.  Blocks the task immediately
// and forces a schedule(), yielding the CPU until wake_at_tick is reached.

static uint64_t sys_sleep(uint64_t ms) {
    if (!g_current_task || ms == 0) return 0;
    g_current_task->state        = TaskState::BLOCKED;
    g_current_task->wake_at_tick = g_tick_count + ms;
    schedule(); // yield NOW — returns when this task is rescheduled
    return 0;
}

static uint64_t sys_spawn(uint64_t path_va) {
    char path[256];
    copy_string_from_user(path, path_va, sizeof(path));
    // Guard: prevent spawning a duplicate interactive init shell
    if (strcmp(path, "/bin/init.elf") == 0 || strcmp(path, "init.elf") == 0) {
        print((char*)"[spawn] Refused: cannot spawn duplicate init shell\n");
        return (uint64_t)-1;
    }
    Task* t = execve_path(path);
    if (!t) return (uint64_t)-1;
    scheduler_add_task(t);
    return (uint64_t)t->pid;
}

extern "C" __attribute__((used))
uint64_t syscall_dispatch(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    switch (nr) {
        case SYS_READ:   return sys_read(a1, a2, a3);
        case SYS_WRITE:  return sys_write(a1, a2, a3);
        case SYS_OPEN:   return sys_open(a1, a2);
        case SYS_CLOSE:  return sys_close(a1);
        case SYS_SCROLL: return sys_scroll((int64_t)a1);
        case SYS_TTYRAW: return sys_ttyraw(a1);
        case SYS_SLEEP:  return sys_sleep(a1);
        case SYS_GETPID: return sys_getpid();
        case SYS_READDIR: return sys_readdir(a1, a2, a3, a4, a5);
        case SYS_SPAWN:   return sys_spawn(a1);
        case SYS_EXIT:   sys_exit(a1);
        case SYS_SHUTDOWN: return sys_shutdown();
        case SYS_REBOOT:   hardware_reset(); return 0;
        
        case SYS_WIN_CREATE: return sys_win_create(a1, a2, a3, a4, a5, 0); // 6th arg flags omitted for now or added
        case SYS_WIN_SYNC:   return sys_win_sync(a1);
        case SYS_WIN_DESTROY: return sys_win_destroy(a1);
        case SYS_WIN_GET_BUFFER: return sys_win_get_buffer(a1);
        case SYS_WIN_DAMAGE: return sys_win_damage(a1, a2, a3, a4, a5);
        
        default:         return (uint64_t)-1;
    }
}

// ── syscall_entry (naked) ─────────────────────────────────────────────────────
//
// CPU state on arrival (set by the SYSCALL instruction):
//   RCX = user RIP,  R11 = user RFLAGS,  RSP = STILL user stack
//   CS/SS switched to kernel selectors,  IF cleared by SFMASK
//
// We must save/restore: RCX, R11 (clobbered by SYSCALL/SYSRET),
// and callee-saved regs: RBP, RBX, R12-R15.
// RAX carries the return value; RDI/RSI/RDX are argument registers.

// ── syscall_entry (Pure Assembly) ─────────────────────────────────────────────
//
// CPU state on arrival:
//   RCX = user RIP,  R11 = user RFLAGS,  RSP = user stack
//   CS=0x08, SS=0x10, IF=0
//
// We use a global asm block to ensure the compiler cannot clobber RCX/R11
// before we push them. Addressing of globals is done via rip-relative [rel].

__asm__ (
    ".globl syscall_entry\n"
    "syscall_entry:\n"
    // 1. Interrupts are disabled on entry (MSR_SFMASK cleared IF).
    // Stash user RSP in scratch while switching to the task's private kernel stack.
    "movq %rsp, g_user_rsp_scratch(%rip)\n"

    // 2. Switch to the current task's private kernel stack.
    "movq g_current_kstack_top(%rip), %rsp\n"

    // 3. Push user RSP onto THIS task's private stack.
    // User RSP is now safely preserved per-task on its own stack frame.
    "pushq g_user_rsp_scratch(%rip)\n"

    // 4. Push user register state for safe keeping.
    "pushq %r11\n"   // user RFLAGS (from SYSCALL)
    "pushq %rcx\n"   // user RIP (from SYSCALL)
    "pushq %rbp\n"
    "pushq %rbx\n"
    "pushq %r15\n"
    "pushq %r14\n"
    "pushq %r13\n"
    "pushq %r12\n"

    // 5. 16-byte stack alignment:
    // We pushed 9 items (9 * 8 = 72 bytes). Sub 8 bytes makes it 80 bytes (80 % 16 == 0).
    "subq $8, %rsp\n"

    // 6. Re-enable interrupts: we are securely on this task's private kernel stack.
    "sti\n"

    // 7. Pass arguments to C++ dispatcher (SysV AMD64 ABI).
    // nr:RAX -> RDI
    // a1:RDI -> RSI, a2:RSI -> RDX, a3:RDX -> RCX, a4:R10 -> R8, a5:R8 -> R9
    "movq %r8,  %r9\n"
    "movq %r10, %r8\n"
    "movq %rdx, %rcx\n"
    "movq %rsi, %rdx\n"
    "movq %rdi, %rsi\n"
    "movq %rax, %rdi\n"
    "call syscall_dispatch\n"

    // 8. Disable interrupts before restoring user state to prevent stack corruption.
    "cli\n"

    // 9. Remove 16-byte alignment padding.
    "addq $8, %rsp\n"

    // 10. Restore user register state from the private stack.
    "popq %r12\n"
    "popq %r13\n"
    "popq %r14\n"
    "popq %r15\n"
    "popq %rbx\n"
    "popq %rbp\n"
    "popq %rcx\n"   // restored user RIP
    "popq %r11\n"   // restored user RFLAGS

    // 11. Restore user stack pointer directly from the private stack.
    "popq %rsp\n"

    // 12. Return to Ring 3.
    "sysretq\n"
);

extern "C" void syscall_entry(); // declaration for C++


// ── syscall_init ──────────────────────────────────────────────────────────────
// Program the MSRs.  Call after init_gdt(), before entering ring 3.

static void syscall_init() {
    // Enable SCE (System Call Extensions) in EFER.
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1ULL);

    // STAR: kernel CS base in [47:32], user CS base in [63:48].
    // SYSCALL  → CS=0x08, SS=0x10  (kernel code/data)
    // SYSRETQ  → CS=0x10+16=0x20|3, SS=0x10+8=0x18|3  (user code/data)
    wrmsr(MSR_STAR, ((uint64_t)0x0010u << 48) | ((uint64_t)0x0008u << 32));

    // LSTAR: entry point for SYSCALL.
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);

    // SFMASK: clear IF on SYSCALL entry so we save registers with interrupts off.
    wrmsr(MSR_SFMASK, 1u << 9);

    // TSS.rsp[0]: kernel stack for hardware interrupts arriving in ring 3.
    g_tss.rsp[0] = g_current_kstack_top;

    print((char*)"[syscall] SYSCALL/SYSRET armed.");
}

// ── enter_usermode ────────────────────────────────────────────────────────────
// Transfer execution to ring 3 at entry_va with RSP = user_stack_va.
// Builds an IRETQ frame on the current kernel stack and fires it.
// Frame (high → low on stack):
//   SS     SEG_USER_DATA | 3  (0x1B)
//   RSP    user_stack_va
//   RFLAGS 0x202  (IF=1, reserved bit 1 set)
//   CS     SEG_USER_CODE | 3  (0x23)
//   RIP    entry_va

[[noreturn]] static void enter_usermode(uint64_t entry_va, uint64_t user_stack_va) {
    const uint64_t ucs    = SEG_USER_CODE | 3u;   // 0x23
    const uint64_t uss    = SEG_USER_DATA | 3u;   // 0x1B
    const uint64_t rflags = 0x202ULL;             // IF=1

    __asm__ volatile (
        "push %[ss]        \n"
        "push %[rsp_u]     \n"
        "push %[rfl]       \n"
        "push %[cs]        \n"
        "push %[rip]       \n"

        // Zero all GP registers so user sees a clean slate.
        "xor %%rax, %%rax  \n"
        "xor %%rbx, %%rbx  \n"
        "xor %%rcx, %%rcx  \n"
        "xor %%rdx, %%rdx  \n"
        "xor %%rdi, %%rdi  \n"
        "xor %%rsi, %%rsi  \n"
        "xor %%rbp, %%rbp  \n"
        "xor %%r8,  %%r8   \n"
        "xor %%r9,  %%r9   \n"
        "xor %%r10, %%r10  \n"
        "xor %%r11, %%r11  \n"
        "xor %%r12, %%r12  \n"
        "xor %%r13, %%r13  \n"
        "xor %%r14, %%r14  \n"
        "xor %%r15, %%r15  \n"

        "iretq             \n"
        :
        : [rip]   "r"(entry_va),
          [cs]    "r"(ucs),
          [rfl]   "r"(rflags),
          [rsp_u] "r"(user_stack_va),
          [ss]    "r"(uss)
        : "memory"
    );
    __builtin_unreachable();
}

#endif // syscall_h