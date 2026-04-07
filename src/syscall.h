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

// ── Syscall kernel stack ──────────────────────────────────────────────────────

#define SYSCALL_KSTACK_SIZE (64u * 1024u)
extern uint8_t g_syscall_kstack[SYSCALL_KSTACK_SIZE];
extern uint64_t g_saved_user_rsp;

#define SYSCALL_KSTACK_TOP \
    ((uint64_t)(g_syscall_kstack + SYSCALL_KSTACK_SIZE) - 8u)


// ── Syscall numbers ───────────────────────────────────────────────────────────

#define SYS_READ    0u
#define SYS_WRITE   1u
#define SYS_OPEN    2u
#define SYS_CLOSE   3u
#define SYS_SCROLL  16u
#define SYS_TTYRAW  17u
#define SYS_GETPID  39u
#define SYS_EXIT    60u
#define SYS_SHUTDOWN 88u
#define SYS_REBOOT   169u

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

// ── syscall_dispatch ──────────────────────────────────────────────────────────
// Called from the naked entry stub.  Returns the value to place in RAX.

extern "C" __attribute__((used))
uint64_t syscall_dispatch(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t user_rip, uint64_t user_rflags) {
    switch (nr) {
        case SYS_READ:   return sys_read(a1, a2, a3);
        case SYS_WRITE:  return sys_write(a1, a2, a3);
        case SYS_OPEN:   return sys_open(a1, a2);
        case SYS_CLOSE:  return sys_close(a1);
        case SYS_SCROLL: return sys_scroll((int64_t)a1);
        case SYS_TTYRAW: return sys_ttyraw(a1);
        case SYS_GETPID: return sys_getpid();
        case SYS_EXIT:   sys_exit(a1);
        case SYS_SHUTDOWN: return sys_shutdown();
        case SYS_REBOOT:   hardware_reset(); return 0;
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
    // 1. Save user RSP to a global slot.
    "movq %rsp, g_saved_user_rsp(%rip)\n"

    // 2. Switch to the dedicated kernel syscall stack.
    //    We calculate the top: g_syscall_kstack + 64KB - 8
    "leaq g_syscall_kstack(%rip), %rsp\n"
    "addq $65528, %rsp\n"  // 64*1024 - 8

    "sti\n"   // ← RE-ENABLE INTERRUPTS — we're on the kernel stack now, it's safe

    // 3. Push user state for safe keeping.
    "pushq %r11\n"   // user RFLAGS
    "pushq %rcx\n"   // user RIP
    "pushq %rbp\n"
    "pushq %rbx\n"
    "pushq %r15\n"
    "pushq %r14\n"
    "pushq %r13\n"
    "pushq %r12\n"

    // 4. Pass arguments to C++ dispatcher.
    // nr:RAX -> RDI, a1:RDI -> RSI, a2:RSI -> RDX, a3:RDX -> RCX
    // rip:RCX -> R8, rflags:R11 -> R9
    "movq %r11, %r9\n"
    "movq %rcx, %r8\n"
    "movq %rdx, %rcx\n"
    "movq %rsi, %rdx\n"
    "movq %rdi, %rsi\n"
    "movq %rax, %rdi\n"
    "call syscall_dispatch\n"

    // 5. Restore user state.
    "popq %r12\n"
    "popq %r13\n"
    "popq %r14\n"
    "popq %r15\n"
    "popq %rbx\n"
    "popq %rbp\n"
    "popq %rcx\n"   // restored user RIP
    "popq %r11\n"   // restored user RFLAGS

    // 6. Restore user stack.
    "movq g_saved_user_rsp(%rip), %rsp\n"

    // 7. Return to Ring 3.
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
    g_tss.rsp[0] = SYSCALL_KSTACK_TOP;

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