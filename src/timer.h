#ifndef timer_h
#define timer_h

// ── timer.h ───────────────────────────────────────────────────────────────────
// PIT (Programmable Interval Timer, Intel 8253/8254) driver.
//
// The PIT has a fixed input clock of 1,193,182 Hz.  We configure channel 0 in
// "rate generator" mode (mode 2) so it generates IRQ 0 at a chosen frequency.
// The IRQ 0 handler increments g_tick_count; callers use timer_sleep_ms() for
// coarse delays or read g_tick_count directly for elapsed-time tracking.
//
// Tick rate: 1000 Hz → 1 ms resolution.  This is fine for a single-core kernel
// without power management, and gives simple ms ↔ tick arithmetic.
// (Linux uses 250 Hz by default; embedded kernels often use 1000 Hz.)
//
// IMPORTANT: Because timer_sleep_ms() uses the HLT instruction, interrupts must
// be enabled (sti) before it is called, otherwise the kernel deadlocks.
//
// USAGE:
//   Call init_timer(1000) after init_irq() and before sti.
//   After sti, timer_sleep_ms(N) waits approximately N milliseconds.
//   g_tick_count counts IRQ 0 firings since boot.
//
// DEPENDENCIES:
//   irq.h  (outb, irq_register) — must be included before this file.
// ──────────────────────────────────────────────────────────────────────────────

#include <stdint.h>
#include "irq.h"

// ── PIT I/O port addresses ────────────────────────────────────────────────────
#define PIT_CH0_DATA  0x40u   // Channel 0 data register (connected to IRQ 0)
#define PIT_CMD_REG   0x43u   // Mode / command register (write-only)

// PIT base oscillator frequency in Hz (defined by hardware; do not change).
#define PIT_BASE_HZ   1193182ULL

// ── Global tick counter ───────────────────────────────────────────────────────
// Incremented once per IRQ 0 (PIT channel 0 interrupt).
// At 1000 Hz this is a millisecond counter.  At 2^64 ticks / 1000 Hz it rolls
// over in ~585 million years — not a concern in practice.
//
// Declared volatile so the compiler re-reads it from memory every time
// timer_sleep_ms() loops, rather than hoisting the read out of the loop.
static volatile uint64_t g_tick_count = 0;

// ── Stored tick rate (set by init_timer) ─────────────────────────────────────
// Kept here so timer_sleep_ms() can convert milliseconds to ticks without
// being re-initialised or passed the frequency at every call.
static uint32_t g_timer_hz = 0;

// ── _timer_irq_handler ────────────────────────────────────────────────────────
// Called from exception_dispatch() (via g_irq_handlers[0]) on every IRQ 0.
// Keep this short: increment the counter and return.  EOI is sent automatically
// by exception_dispatch() — do NOT call it here.
static void _timer_irq_handler() {
    g_tick_count++;
}

// ── _pit_set_hz ───────────────────────────────────────────────────────────────
// Internal helper: configure PIT channel 0 to fire at `hz` interrupts/second.
//
// Command byte format (sent to PIT_CMD_REG):
//   bits 7-6: channel select (00 = channel 0)
//   bits 5-4: access mode   (11 = lo/hi byte)
//   bits 3-1: operating mode (010 = rate generator — square wave on some chips)
//   bit    0: BCD/binary    (0 = binary)
//   → 0b_00_11_010_0 = 0x34 for channel 0 rate generator
//
// Divisor: PIT counts down from the divisor and fires when it reaches zero.
//   divisor = PIT_BASE_HZ / desired_hz
// Clamped to [1, 0xFFFF] — the hardware reload register is 16 bits, and 0
// means 65536 (lowest possible rate, ~18.2 Hz).
static void _pit_set_hz(uint32_t hz) {
    if (hz == 0) hz = 1;

    uint32_t divisor = (uint32_t)(PIT_BASE_HZ / (uint64_t)hz);
    if (divisor == 0)      divisor = 1;
    if (divisor > 0xFFFF)  divisor = 0xFFFF;

    // Write command byte: channel 0, lo/hi byte access, rate generator, binary.
    outb(PIT_CMD_REG, 0x36);

    // Write divisor low byte then high byte (lo/hi access mode).
    outb(PIT_CH0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0_DATA, (uint8_t)(divisor >> 8));
}

// ── timer_sleep_ticks ─────────────────────────────────────────────────────────
// Busy-wait for exactly `ticks` timer interrupts.
// Uses HLT so the CPU can idle rather than spinning — requires interrupts to be
// enabled or this function never returns.
static void timer_sleep_ticks(uint64_t ticks) {
    uint64_t target = g_tick_count + ticks;
    while (g_tick_count < target) {
        // HLT suspends the CPU until the next interrupt fires the tick handler.
        // The 'memory' clobber prevents the compiler from caching g_tick_count.
        __asm__ volatile ("hlt" ::: "memory");
    }
}

// ── timer_sleep_ms ────────────────────────────────────────────────────────────
// Busy-wait for approximately `ms` milliseconds.
// Resolution is ±1 tick (±1 ms at 1000 Hz).  For sub-millisecond waits, use
// tsc_delay_ms() from usbhelpers.h instead.
//
// Requires: init_timer() has been called and interrupts are enabled (sti).
static void timer_sleep_ms(uint32_t ms) {
    if (g_timer_hz == 0) return;   // timer not yet initialised
    uint64_t ticks = (uint64_t)ms * (uint64_t)g_timer_hz / 1000ULL;
    if (ticks == 0) ticks = 1;     // always wait at least one tick
    timer_sleep_ticks(ticks);
}

// ── timer_uptime_ms ───────────────────────────────────────────────────────────
// Return elapsed milliseconds since init_timer() was called.
// Equivalent to g_tick_count when using a 1000 Hz timer.
static uint64_t timer_uptime_ms() {
    if (g_timer_hz == 0) return 0;
    return g_tick_count * 1000ULL / (uint64_t)g_timer_hz;
}

// ── init_timer ────────────────────────────────────────────────────────────────
// Configure PIT channel 0 at `hz` Hz and install the IRQ 0 handler.
//
// Recommended: init_timer(1000) for 1 ms ticks.
// Call after init_irq(), before enabling interrupts (sti).
static void init_timer(uint32_t hz) {
    g_timer_hz = hz;
    _pit_set_hz(hz);
    irq_register(0, _timer_irq_handler);   // IRQ 0 = PIT channel 0

    char buf[32];
    print((char*)"Timer: PIT channel 0 at");
    to_str(hz, buf); print(buf);
    print((char*)"Hz. 1 tick =");
    to_str(1000 / hz, buf); print(buf);
    print((char*)"ms. IRQ 0 armed.");
}

#endif // timer_h