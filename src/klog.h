#ifndef KLOG_H
#define KLOG_H

#include <stdint.h>
#include "helpers.h"

// ── klog.h ──────────────────────────────────────────────────────────────────
//
// Phase 8 — Kernel Log Buffer
//
// A simple static buffer to hold kernel log messages (print()).
// This allows logs to be captured and viewed later in the userspace terminal
// without cluttering the main display during interactive sessions.
//
// ──────────────────────────────────────────────────────────────────────────────

#define KLOG_BUFFER_SIZE (64 * 1024)
extern char     g_klog_buffer[KLOG_BUFFER_SIZE];
extern uint64_t g_klog_pos;
extern bool     g_klog_bypass_framebuffer;

// Internal: append a single character to the kernel log.
static inline void klog_putc(char c) {
    if (g_klog_pos < KLOG_BUFFER_SIZE - 1) {
        g_klog_buffer[g_klog_pos++] = c;
        g_klog_buffer[g_klog_pos]   = '\0';
    }
}

// Append a null-terminated string to the kernel log.
static inline void klog_print(const char* s) {
    if (!s) return;
    while (*s) {
        klog_putc(*s++);
    }
}

#endif // KLOG_H
