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
#include "app.h"
#include "tty.h"
#include "acpi.h"
#include "wm.h"
#include "app_console.h"

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
#define SYS_STAT    4u
#define SYS_LSEEK   8u
#define SYS_SCROLL  16u
#define SYS_TTYRAW  17u
#define SYS_TTYECHO 18u
#define SYS_TTYFLUSH_INPUT 19u
#define SYS_TTYTAKE_LAUNCH 20u
#define SYS_SLEEP   35u
#define SYS_GETPID  39u
#define SYS_READDIR 40u
#define SYS_SPAWN   59u
#define SYS_EXIT    60u
#define SYS_WAITPID 61u
#define SYS_FSYNC   74u
#define SYS_FTRUNCATE 77u
#define SYS_RENAME 82u
#define SYS_REPLACE 138u
#define SYS_GETCWD  79u
#define SYS_EXEPATH 210u   // path of the calling program (like /proc/self/exe)
#define SYS_EXCHANGE 211u  // atomically swap two closed files in one directory
#define SYS_TRYWAIT 212u   // nonblocking wait for one child; 0 means still running
#define SYS_LAUNCH 213u    // subsystem-aware launch; desktop origin=1, shell origin=0
#define SYS_CONSOLE 214u   // private console-host channel
#define SYS_CHDIR   80u
#define SYS_MKDIR   83u
#define SYS_RMDIR   84u
#define SYS_UNLINK  87u
#define SYS_SHUTDOWN 88u
#define SYS_STATFS  137u
#define SYS_REBOOT   169u

#define SYS_WIN_CREATE    200u
#define SYS_WIN_SYNC      201u
#define SYS_WIN_DESTROY   202u
#define SYS_WIN_GET_BUFFER 203u
#define SYS_WIN_DAMAGE     204u   // (win_id, x, y, w, h): part of the surface changed
#define SYS_WIN_POLL_EVENT 205u

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

static bool get_user_path(uint64_t path_va, char* out_path, uint64_t max_len) {
    if (!path_va || !out_path) return false;
    char raw[256];
    copy_string_from_user(raw, path_va, sizeof(raw));
    const char* cwd = g_current_task ? g_current_task->cwd : "/";
    return vfs_resolve_path(cwd, raw, out_path, max_len);
}

// ── sys_open ──────────────────────────────────────────────────────────────────

static uint64_t sys_open(uint64_t path_va, uint64_t flags) {
    if (!g_current_task) return (uint64_t)-1;
    
    char path[256];
    if (!get_user_path(path_va, path, sizeof(path))) return (uint64_t)-1;
    
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

static int64_t sys_lseek(uint64_t fd, int64_t offset, uint64_t whence) {
    if (!g_current_task || fd >= MAX_FDS) return -1;
    file* f = g_current_task->fd_table[fd];
    if (!f || !f->vn) return -1;
    int64_t new_offset = 0;
    if (whence == 0) { // SEEK_SET
        new_offset = offset;
    } else if (whence == 1) { // SEEK_CUR
        new_offset = (int64_t)f->offset + offset;
    } else if (whence == 2) { // SEEK_END
        new_offset = (int64_t)f->vn->size + offset;
    } else {
        return -1;
    }
    if (new_offset < 0) return -1;
    f->offset = (uint64_t)new_offset;
    return new_offset;
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
        if(g_current_task->console){if(!buf_va||buf_va>=USERSPACE_END||len>USERSPACE_END-buf_va)return (uint64_t)-1;
            return console_read((AppConsole*)g_current_task->console,(char*)buf_va,len);}
        if(g_current_task->gui_app)return 0;
        if (g_current_task->pid==g_terminal_shell_pid) g_terminal_shell_ready=true;
        char tmp[128];
        size_t to_read = len > 128 ? 128 : len;
        size_t read_bytes = tty_read(tmp, to_read);
        if (!read_bytes && tty_shell_launch_ready()) return (uint64_t)-2;
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
        if(g_current_task->console){
            if(!buf_va||buf_va>=USERSPACE_END||len>USERSPACE_END-buf_va)return (uint64_t)-1;
            console_output((AppConsole*)g_current_task->console,(const char*)buf_va,len);return len;
        }
        if(g_current_task->gui_app)return len;
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

[[noreturn]] static void sys_exit(uint64_t code) {
    if (g_current_task) {
        task_exit(code);   // wakes a waiting parent, then schedules away
    }
    hcf();
}

// ── sys_shutdown ──────────────────────────────────────────────────────────────

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static void storage_prepare_poweroff() {
    if (vfs_shutdown_hook && vfs_shutdown_hook() != 0) {
        // A removed/failed root cannot be flushed. It must not prevent the
        // requested power action; leave its journal for next-boot recovery.
        print((char*)"[storage] System filesystem could not close cleanly; continuing power action.");
    }
}

static uint64_t sys_shutdown() {
    storage_prepare_poweroff();
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
    if(g_current_task&&(g_current_task->console||g_current_task->gui_app))return 0;
    term_scroll_view(delta);
    return 0;
}

static uint64_t sys_ttyraw(uint64_t raw) {
    if(g_current_task&&g_current_task->console){((AppConsole*)g_current_task->console)->raw=raw!=0;return 0;}
    if(g_current_task&&g_current_task->gui_app)return 0;
    tty_set_raw(raw != 0);
    return 0;
}

// ── sys_win_create ────────────────────────────────────────────────────────────
static uint64_t sys_win_create(uint64_t title_va, uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint64_t flags) {
    char title[64];
    copy_string_from_user(title, title_va, sizeof(title));
    bool closable = true; // All user windows are closable
    pid_t owner = g_current_task ? g_current_task->pid : 0;
    Window* win = wm_create_window(title, (int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, closable, false, WIN_TYPE_USER, owner);
    if (!win) return 0;

    // Map window surface pages into user process address space (Ring 3 accessible)
    if (g_current_task && g_current_task->cr3 && win->buffer_phys) {
        uint64_t user_va = USER_WIN_BASE + (uint64_t)win->id * USER_WIN_STRIDE;
        win->user_va = user_va;
        for (uint64_t p = 0; p < win->buffer_pages; p++) {
            uint64_t v  = user_va + p * PAGE_SIZE;
            uint64_t pa = win->buffer_phys + p * PAGE_SIZE;
            map_page_in(g_current_task->cr3, v, pa, PRESENT | WRITABLE | USER);
            __asm__ volatile("invlpg (%0)" :: "r"(v) : "memory");
        }
    }
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
    Window* win = wm_find_window((int)win_id);
    if (!win) return 0;
    if (win->user_va) return win->user_va;
    return (uint64_t)win->buffer;
}

// ── sys_win_poll_event ────────────────────────────────────────────────────────
static uint64_t sys_win_poll_event(uint64_t win_id, uint64_t ev_va) {
    if (!ev_va) return 0;
    Window* win = wm_find_window((int)win_id);
    if (!win) return 0;
    WinEvent ev;
    if (!wm_win_pop_event(win, &ev)) return 0;
    WinEvent* u_ev = (WinEvent*)ev_va;
    *u_ev = ev;
    return 1;
}

// ── syscall_dispatch ──────────────────────────────────────────────────────────
// Called from the naked entry stub.  Returns the value to place in RAX.

static uint64_t sys_readdir(uint64_t path_va, uint64_t index, uint64_t name_out_va, uint64_t size_out_va, uint64_t type_out_va) {
    char path[256];
    if (!get_user_path(path_va, path, sizeof(path))) return (uint64_t)-1;
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
static int64_t launch_program(const char* path,bool desktop,bool* gui);

static uint64_t sys_unlink(uint64_t path_va) {
    char path[256];
    if (!get_user_path(path_va, path, sizeof(path))) return (uint64_t)-1;
    return (uint64_t)vfs_unlink(path);
}
static uint64_t sys_stat(uint64_t path_va, uint64_t info_va) {
    if (!info_va || info_va >= USERSPACE_END || sizeof(file_info) > USERSPACE_END - info_va) return (uint64_t)-1;
    char path[256];
    if (!get_user_path(path_va, path, sizeof(path))) return (uint64_t)-1;
    file_info info;
    int result = vfs_stat(path, &info);
    if (result != 0) return (uint64_t)result;
    const uint8_t* src = (const uint8_t*)&info;
    uint8_t* dest = (uint8_t*)info_va;
    for (uint64_t i = 0; i < sizeof(info); ++i) dest[i] = src[i];
    return 0;
}

static uint64_t sys_chdir(uint64_t path_va) {
    if (!g_current_task) return (uint64_t)-1;
    char path[256];
    if (!get_user_path(path_va, path, sizeof(path))) return (uint64_t)-1;
    vnode* vn = vfs_namei(path);
    if (!vn || vn->type != VnodeType::DIRECTORY) return (uint64_t)-1;
    strcpy(g_current_task->cwd, path);
    return 0;
}

// Copy the absolute path of the caller's own executable into buf. Returns its
// length (excluding the NUL) or -1. Lets a portable .wiz open its own bundle.
static uint64_t sys_exepath(uint64_t buf_va, uint64_t size) {
    if (!g_current_task || !buf_va || !size || buf_va >= USERSPACE_END) return (uint64_t)-1;
    const char* path = g_current_task->executable;
    uint64_t len = strlen(path);
    if (!len || size < len + 1 || len + 1 > USERSPACE_END - buf_va) return (uint64_t)-1;
    copy_string_to_user(buf_va, path, len + 1);
    return len;
}

static uint64_t sys_getcwd(uint64_t buf_va, uint64_t size) {
    if (!g_current_task || !buf_va || !size || buf_va >= USERSPACE_END) return 0;
    const char* cwd = g_current_task->cwd[0] ? g_current_task->cwd : "/";
    uint64_t len = strlen(cwd) + 1;
    if (size < len || len > USERSPACE_END - buf_va) return 0;
    copy_string_to_user(buf_va, cwd, len);
    return buf_va;
}

static uint64_t sys_mkdir(uint64_t path_va, uint64_t mode) {
    (void)mode;
    char path[256];
    if (!get_user_path(path_va, path, sizeof(path))) return (uint64_t)-1;
    return (uint64_t)vfs_mkdir(path);
}

static uint64_t sys_rmdir(uint64_t path_va) {
    char path[256];
    if (!get_user_path(path_va, path, sizeof(path))) return (uint64_t)-1;
    if (strcmp(path, "/") == 0) return (uint64_t)-1;
    if (g_current_task && strcmp(path, g_current_task->cwd) == 0) return (uint64_t)-1;
    return (uint64_t)vfs_rmdir(path);
}

static uint64_t sys_statfs(uint64_t path_va, uint64_t buf_va) {
    if (!buf_va || buf_va >= USERSPACE_END || sizeof(fs_info) > USERSPACE_END - buf_va) return (uint64_t)-1;
    char path[256];
    if (path_va) {
        if (!get_user_path(path_va, path, sizeof(path))) return (uint64_t)-1;
    } else {
        strcpy(path, "/");
    }
    fs_info info;
    int res = vfs_statfs(path, &info);
    if (res != 0) return (uint64_t)res;
    const uint8_t* src = (const uint8_t*)&info;
    uint8_t* dest = (uint8_t*)buf_va;
    for (uint64_t i = 0; i < sizeof(info); ++i) dest[i] = src[i];
    return 0;
}

// ── sys_sleep ─────────────────────────────────────────────────────────────────
// Block the calling task for `ms` milliseconds (rounded up to tick granularity).
// At 1000 Hz each tick = 1 ms, so ms == ticks.  Blocks the task immediately
// and forces a schedule(), yielding the CPU until wake_at_tick is reached.

static uint64_t sys_sleep(uint64_t ms) {
    task_sleep_ms(ms);
    return 0;
}

static uint64_t sys_spawn(uint64_t path_va) {
    char path[256];
    if (!get_user_path(path_va, path, sizeof(path))) return (uint64_t)-1;
    // Guard: prevent spawning a duplicate interactive init shell
    if (app_path_is_shell(path)) {
        print((char*)"[spawn] Refused: cannot spawn duplicate init shell\n");
        return (uint64_t)-1;
    }
    Task* t = execve_path(path);
    if (!t) return (uint64_t)-1;
    t->parent_pid = g_current_task ? g_current_task->pid : 0;
    if (g_current_task) {
        strcpy(t->cwd, g_current_task->cwd);
        {TtyIrqGuard guard;t->gui_app=g_current_task->gui_app;
            t->console=g_current_task->console;
            if(t->console){auto* c=(AppConsole*)t->console;++c->refs;if(g_current_task->console_host)c->foreground=t->pid;t->gui_app=false;}}
    }
    if (t->parent_pid == g_terminal_shell_pid) g_terminal_foreground_pid = t->pid;
    scheduler_add_task(t);
    return (uint64_t)t->pid;
}

static uint64_t sys_take_launch(uint64_t dest, uint64_t capacity) {
    if (!tty_shell_launch_ready() || !dest || dest >= USERSPACE_END || capacity > USERSPACE_END - dest) return (uint64_t)-1;
    uint64_t length = strlen(tty_launch_path);
    if (capacity <= length) return (uint64_t)-1;
    for (uint64_t i = 0; i <= length; ++i) ((char*)dest)[i] = tty_launch_path[i];
    tty_launch_pending = false;
    tty_discard_input();
    return length;
}
static uint64_t syscall_dispatch_body(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
extern "C" __attribute__((used))
uint64_t syscall_dispatch(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    Task* caller = g_current_task;
    if (caller) {
        caller->in_syscall = true;
        if (caller->interrupt_requested) sys_exit(130);
    }
    uint64_t result = syscall_dispatch_body(nr, a1, a2, a3, a4, a5);
    if (caller) {
        if (caller->interrupt_requested) sys_exit(130);
        caller->in_syscall = false;
    }
    return result;
}
static uint64_t syscall_dispatch_body(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    switch (nr) {
        case SYS_READ:   return sys_read(a1, a2, a3);
        case SYS_WRITE:  return sys_write(a1, a2, a3);
        case SYS_OPEN:   return sys_open(a1, a2);
        case SYS_CLOSE:  return sys_close(a1);
        case SYS_FSYNC:
            return g_current_task && a1 < MAX_FDS ? vfs_fsync(g_current_task->fd_table[a1]) : (uint64_t)-1;
        case SYS_FTRUNCATE:
            return g_current_task && a1 < MAX_FDS ? vfs_ftruncate(g_current_task->fd_table[a1], a2) : (uint64_t)-1;
        case SYS_EXCHANGE:
        case SYS_RENAME: {
            char path[256], destination[256];
            if (!get_user_path(a1, path, sizeof(path)) || !get_user_path(a2, destination, sizeof(destination))) return (uint64_t)-1;
            return nr == SYS_EXCHANGE ? vfs_exchange(path, destination) : vfs_rename(path, destination);
        }
        case SYS_REPLACE: {
            char path[256];
            if (!get_user_path(a1, path, sizeof(path))) return (uint64_t)-1;
            return vfs_replace(path, (const void*)a2, a3);
        }
        case SYS_STAT:   return sys_stat(a1, a2);
        case SYS_LSEEK:  return (uint64_t)sys_lseek(a1, (int64_t)a2, a3);
        case SYS_UNLINK: return sys_unlink(a1);
        case SYS_SCROLL: return sys_scroll((int64_t)a1);
        case SYS_TTYRAW: return sys_ttyraw(a1);
        case SYS_TTYECHO:
            if(g_current_task&&g_current_task->console){auto* c=(AppConsole*)g_current_task->console;bool old=c->echo;c->echo=a1!=0;return old;}
            return g_current_task&&g_current_task->gui_app?0:tty_set_echo(a1 != 0) ? 1 : 0;
        case SYS_TTYFLUSH_INPUT:
            if(g_current_task&&g_current_task->console){auto* c=(AppConsole*)g_current_task->console;TtyIrqGuard guard;c->input_tail=c->input_head;c->line_len=0;}
            else if(!g_current_task||!g_current_task->gui_app)tty_discard_input();return 0;
        case SYS_TTYTAKE_LAUNCH: return sys_take_launch(a1, a2);
        case SYS_SLEEP:  return sys_sleep(a1);
        case SYS_GETPID: return sys_getpid();
        case SYS_READDIR: return sys_readdir(a1, a2, a3, a4, a5);
        case SYS_SPAWN:   return sys_spawn(a1);
        case SYS_LAUNCH: {
            char path[256];if(a2>1||!get_user_path(a1,path,sizeof(path))||
                (a3&&(a3>=USERSPACE_END||sizeof(uint64_t)>USERSPACE_END-a3)))return (uint64_t)-1;
            bool gui=false;int64_t pid=launch_program(path,a2==1,&gui);
            if(pid>0&&a3)*(uint64_t*)a3=gui?1:0;return pid;
        }
        case SYS_CONSOLE:return sys_console(a1,a2,a3);
        case SYS_EXIT:   sys_exit(a1);
        case SYS_TRYWAIT:
        case SYS_WAITPID: {
            if (a1 > 0xffffffffULL || (a2 && (a2 >= USERSPACE_END || sizeof(uint64_t) > USERSPACE_END - a2))) return (uint64_t)-1;
            uint64_t code = 0;
            int64_t pid = task_wait_child((pid_t)a1, &code, nr == SYS_TRYWAIT);
            if (pid > 0 && a2) *(uint64_t*)a2 = code;
            return (uint64_t)pid;
        }
        case SYS_GETCWD: return sys_getcwd(a1, a2);
        case SYS_EXEPATH: return sys_exepath(a1, a2);
        case SYS_CHDIR:  return sys_chdir(a1);
        case SYS_MKDIR:  return sys_mkdir(a1, a2);
        case SYS_RMDIR:  return sys_rmdir(a1);
        case SYS_STATFS: return sys_statfs(a1, a2);
        case SYS_SHUTDOWN: return sys_shutdown();
        case SYS_REBOOT:
            storage_prepare_poweroff();
            hardware_reset(); return 0;
        
        case SYS_WIN_CREATE: return sys_win_create(a1, a2, a3, a4, a5, 0); // 6th arg flags omitted for now or added
        case SYS_WIN_SYNC:   return sys_win_sync(a1);
        case SYS_WIN_DESTROY: return sys_win_destroy(a1);
        case SYS_WIN_GET_BUFFER: return sys_win_get_buffer(a1);
        case SYS_WIN_DAMAGE: return sys_win_damage(a1, a2, a3, a4, a5);
        case SYS_WIN_POLL_EVENT: return sys_win_poll_event(a1, a2);
        
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
    // Inline user syscall wrappers declare only RAX/RCX/R11 clobbered.
    // Preserve their argument/scratch registers across the C dispatcher too.
    "pushq %rdi\n"
    "pushq %rsi\n"
    "pushq %rdx\n"
    "pushq %r8\n"
    "pushq %r9\n"
    "pushq %r10\n"

    // 5. 16-byte stack alignment:
    // Fifteen pushes = 120 bytes; another 8 bytes aligns the C call.
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
    "popq %r10\n"
    "popq %r9\n"
    "popq %r8\n"
    "popq %rdx\n"
    "popq %rsi\n"
    "popq %rdi\n"
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
