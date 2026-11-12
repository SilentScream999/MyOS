#ifndef TTY_H
#define TTY_H

#include <stdint.h>
#include <stddef.h>
#include "terminal.h"
#include "scheduler.h"

#define TTY_BUF_SIZE 4096

// The raw input buffer accumulates characters as typed.
static char tty_line_buf[TTY_BUF_SIZE];
static uint32_t tty_line_len = 0;
static bool tty_raw_mode = false;
static bool tty_echo_enabled = true;
static char tty_launch_path[124];
static bool tty_launch_pending = false;
static volatile bool g_terminal_shell_ready = false; // Shell reached its first input request.

static bool tty_queue_launch(const char* path) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    size_t len = 0;
    while (path && path[len] && len < sizeof(tty_launch_path)) ++len;
    bool accepted = g_terminal_shell_pid && !g_terminal_foreground_pid && !tty_launch_pending &&
                    path && path[0] == '/' && len && len < sizeof(tty_launch_path);
    if (accepted) {
        for (size_t i = 0; i <= len; ++i) tty_launch_path[i] = path[i];
        tty_launch_pending = true;
    }
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
    return accepted;
}
static bool tty_shell_launch_ready() {
    return tty_launch_pending && g_current_task && g_current_task->pid == g_terminal_shell_pid;
}

static bool tty_set_echo(bool enabled) {
    bool previous = tty_echo_enabled;
    tty_echo_enabled = enabled;
    return previous;
}

static void tty_set_raw(bool raw) {
    tty_raw_mode = raw;
}

// The canonical output buffer holds fully assembled lines ready to be read.
static char tty_out_buf[TTY_BUF_SIZE];
static volatile uint32_t tty_out_head = 0;
static volatile uint32_t tty_out_tail = 0;

static void tty_discard_input() {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    tty_line_len = 0;
    tty_out_tail = tty_out_head;
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

static void tty_flush_line() {
    for (uint32_t i = 0; i < tty_line_len; i++) {
        uint32_t next = (tty_out_head + 1) % TTY_BUF_SIZE;
        if (next != tty_out_tail) {
            tty_out_buf[tty_out_head] = tty_line_buf[i];
            tty_out_head = next;
        }
    }
    tty_line_len = 0;
}

static void tty_input(char c) {
    if (tty_raw_mode) {
        // Raw mode: forward every character straight to the shell, no echo, no editing.
        uint32_t next = (tty_out_head + 1) % TTY_BUF_SIZE;
        if (next != tty_out_tail) {
            tty_out_buf[tty_out_head] = c;
            tty_out_head = next;
        }
        return; // no term_flip needed; shell will echo via sys_write
    }

    // Canonical mode: kernel handles line-editing and echo.
    if (c == '\b') {
        if (tty_line_len > 0) {
            tty_line_len--;
            if (tty_echo_enabled) term_putchar('\b');
        }
    } else {
        if (tty_echo_enabled && (c >= 32 || c == '\n' || c == '\r' || c == '\t')) {
            term_putchar(c);
        }
        if (tty_line_len < TTY_BUF_SIZE - 2) {
            if (c == '\r') c = '\n';
            tty_line_buf[tty_line_len++] = c;
            if (c == '\n') tty_flush_line();
        }
    }
    if (tty_echo_enabled) term_flip();
}

// In kernel reads. Blocks until at least 1 character is available.
static size_t tty_read(char* buf, size_t count) {
    size_t read = 0;
    while (read < count) {
        if ((g_current_task && g_current_task->interrupt_requested) || tty_shell_launch_ready()) break;
        if (tty_out_head != tty_out_tail) {
            char c = tty_out_buf[tty_out_tail];
            tty_out_tail = (tty_out_tail + 1) % TTY_BUF_SIZE;
            buf[read++] = c;
            // In Raw mode, return every character immediately.
            // In Canonical mode, wait for newline.
            if (c == '\n' || tty_raw_mode) break; 
        } else {
            // No data available yet.
            if (read > 0) break; // Return what we have if we got a partial line
            
            // Yield CPU to other tasks (specifically kmain, to poll USB)
            yield();
        }
    }
    return read;
}

// TTY write: shell output path. Snaps view to live and draws cursor after flush.
static size_t tty_write(const char* buf, size_t count) {
    if (g_term_view_offset != 0) {
        g_term_view_offset = 0;    // snap back to live view on any shell output
        term_sync_backbuffer();    // Repopulate pixel buffer
        term_dirty_all();
    }
    term_write(buf, count);  // writes chars, flips backbuffer → VRAM
    // Only draw cursor when shell owns the keyboard (raw/interactive mode).
    // During boot prints and command output, no cursor should appear.
    if (tty_raw_mode) {
        term_render_cursor();
    }
    return count;
}

#endif // TTY_H
