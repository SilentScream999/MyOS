#ifndef timer_h
#define timer_h

// ── timer.h ───────────────────────────────────────────────────────────────────
//
// PIT (8253/8254) channel 0 driver.
//
// Provides:
//   init_timer(hz)  — program the PIT and register the IRQ 0 handler.
//   g_tick_count    — millisecond counter incremented every tick.
//   sleep_ms(ms)    — busy-wait delay in whole milliseconds.
//
// SCHEDULER INTEGRATION
//   The IRQ 0 handler sends PIC EOI *before* calling scheduler_tick() so that
//   the PIC is ready to deliver the next timer interrupt regardless of which
//   task the scheduler switches to.  If EOI were sent by exception_dispatch()
//   *after* the handler returns, a context switch would leave IRQ0 "in service"
//   in the PIC until the original task was eventually rescheduled — freezing
//   timer ticks for every other task.
//
// HOW IT FITS TOGETHER
//   irq.h       — outb/inb, irq_register, PIC constants
//   idt.h       — irq_handler_t, exception_dispatch (calls handler then EOI)
//   scheduler.h — scheduler_tick() (called here, after EOI)
//
// NOTE
//   Because this file sends its own EOI, exception_dispatch() will send a
//   *second* EOI for IRQ0 after the handler returns.  A redundant non-specific
//   EOI to the master PIC is harmless: if no interrupt is in service the PIC
//   ignores it.  If that ever changes, add a "skip EOI" flag to the IRQ
//   dispatch table.
//
// DEPENDENCIES
//   irq.h       — must be included first (defines outb/inb, irq_register)
//   scheduler.h — included here; do not include scheduler.h before timer.h
// ──────────────────────────────────────────────────────────────────────────────

#include <stdint.h>
#include "irq.h"        // outb, inb, irq_register, PIC1_CMD, PIC_EOI
#include "scheduler.h"  // scheduler_tick()

// ── PIT port / mode constants ─────────────────────────────────────────────────

#define PIT_CHANNEL0  0x40u   // Channel 0 data port
#define PIT_CMD       0x43u   // Mode / command register

// Command byte: channel 0, lobyte/hibyte access, mode 2 (rate generator), binary
#define PIT_CMD_RATE  0x36u

// PIT input frequency (Hz) — fixed by hardware.
#define PIT_BASE_HZ   1193182ULL

// ── Global tick counter ────────────────────────────────────────────────────────
// Incremented once per tick; at 1000 Hz this is a millisecond counter.
// Declared volatile so the compiler never caches it in a register.

static volatile uint64_t g_tick_count = 0;

// ── timer_irq_handler ─────────────────────────────────────────────────────────
// Called by exception_dispatch() on every IRQ 0 (PIT channel 0 interrupt).
//
// ORDER IS CRITICAL:
//   1. Send EOI first — the PIC can then accept the next timer interrupt
//      even if schedule() switches us away before this function returns.
//   2. Increment the tick counter.
//   3. Call scheduler_tick() — may or may not call schedule() internally.

static void timer_irq_handler() {
    // 1. Acknowledge the interrupt at the PIC BEFORE potentially switching tasks.
    //    This is safe because PIC interrupts are still disabled (the CPU cleared
    //    IF on interrupt entry) — no reentrance is possible.
    outb(PIC1_CMD, PIC_EOI);

    // 2. Advance the wall-clock tick counter.
    g_tick_count++;

    // 3. Give the scheduler a chance to preempt the current task.
    //    At 1000 Hz with SCHEDULER_TICKS = 10 this fires every 10 ms.
    //    If schedule() switches context, execution resumes here on the NEXT
    //    occasion this task is scheduled back in — which is fine because EOI
    //    was already sent above.
    scheduler_tick();
}

// ── init_timer ────────────────────────────────────────────────────────────────
// Program the PIT to fire at `hz` interrupts per second and register the
// handler on IRQ 0.
//
// Call after init_irq() and before __asm__ volatile ("sti").

static void init_timer(uint32_t hz) {
    if (hz == 0) hz = 1;

    // Compute divisor; clamp to [1, 65535].
    uint32_t divisor = (uint32_t)(PIT_BASE_HZ / hz);
    if (divisor < 1)      divisor = 1;
    if (divisor > 0xFFFF) divisor = 0xFFFF;

    // Program the PIT.
    outb(PIT_CMD,      PIT_CMD_RATE);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));        // low byte
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF)); // high byte

    // Register and unmask IRQ 0.
    irq_register(0, timer_irq_handler);

    char str[32];
    print((char*)"Timer: PIT channel 0 at ");
    to_str(hz, str); print(str);
    print((char*)"Hz.");
}

// ── sleep_ms ──────────────────────────────────────────────────────────────────
// Busy-wait for at least `ms` milliseconds using g_tick_count.
// Interrupts must be enabled for this to advance.

static void sleep_ms(uint64_t ms) {
    uint64_t target = g_tick_count + ms;
    while (g_tick_count < target) {
        __asm__ volatile ("pause");
    }
}

#endif // timer_h