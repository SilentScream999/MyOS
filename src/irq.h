#ifndef irq_h
#define irq_h

// ── irq.h ─────────────────────────────────────────────────────────────────────
// 8259A PIC remapping and pluggable IRQ handler system.
//
// WHY REMAP THE PIC?
//   After reset, the BIOS leaves IRQ 0-15 mapped to CPU vectors 8-15/0x70-0x77.
//   Vectors 8-15 overlap the CPU exception range (#DF, #TS, #NP, #SS, #GP,
//   #PF, #MF) — so a harmless timer tick looks like a double-fault.  We remap
//   IRQ 0-7  → vectors 32-39  and
//   IRQ 8-15 → vectors 40-47
//   safely above all 32 CPU exception vectors.
//
// HOW IT FITS TOGETHER:
//   idt.h         — defines irq_handler_t, g_irq_handlers[16], _pic_eoi(),
//                   ISR stubs 32-47, and the dispatch logic inside
//                   exception_dispatch().
//   irq.h (here)  — initialises the PIC and provides irq_register / irq_unregister
//                   so driver code never touches PIC ports directly.
//   timer.h       — calls irq_register(0, ...) to wire up the PIT IRQ.
//
// USAGE:
//   1. Call init_irq() once, after init_idt(), before enabling interrupts.
//   2. Register handlers: irq_register(irq_line_0_to_15, your_handler);
//   3. Enable interrupts:  __asm__ volatile ("sti");
//
//   Handlers registered here do NOT send EOI — exception_dispatch() does that
//   automatically after calling the handler.
//
// NOTE: outb / inb are defined here because they are first needed by the PIC
// initialisation routines.  timer.h and any future driver headers include
// irq.h to access them.
// ──────────────────────────────────────────────────────────────────────────────

#include <stdint.h>
#include "helpers.h"
#include "idt.h"    // irq_handler_t, g_irq_handlers[]

// ── Port I/O primitives ───────────────────────────────────────────────────────
// These are the canonical outb/inb definitions for the whole kernel.
// Every file that needs port I/O should include irq.h (directly or indirectly).

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

// ~1 µs I/O delay — write to unused diagnostic port 0x80.
// Required between consecutive PIC initialisation commands on real hardware.
static inline void io_wait() { outb(0x80, 0); }

// ── 8259A PIC register addresses ─────────────────────────────────────────────
#define PIC1_CMD   0x20u   // Master PIC command port
#define PIC1_DATA  0x21u   // Master PIC data / IMR port
#define PIC2_CMD   0xA0u   // Slave  PIC command port
#define PIC2_DATA  0xA1u   // Slave  PIC data / IMR port
#define PIC_EOI    0x20u   // Non-specific End-Of-Interrupt command byte

// Vector base addresses chosen to avoid the 32 CPU exception vectors.
#define IRQ_BASE    32u    // IRQ 0-7  → vectors 32-39
#define IRQ_BASE_S  40u    // IRQ 8-15 → vectors 40-47

// ── irq_register ──────────────────────────────────────────────────────────────
// Install a C-callable handler for IRQ line 0-15 and unmask that line in the
// PIC's Interrupt Mask Register (IMR).
//
// The handler is invoked with interrupts disabled (the IDT gate clears IF).
// It must NOT send EOI — exception_dispatch() does that for you.
static void irq_register(uint8_t irq, irq_handler_t handler) {
    if (irq >= 16) return;
    g_irq_handlers[irq] = handler;

    // Compute which PIC and which bit controls this IRQ line.
    uint16_t port = (irq < 8u) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (irq < 8u) ? irq : (uint8_t)(irq - 8u);

    // Clear the mask bit to enable the IRQ line.
    outb(port, (uint8_t)(inb(port) & ~(1u << bit)));
}

// ── irq_unregister ────────────────────────────────────────────────────────────
// Remove a handler and re-mask the IRQ line so no spurious events accumulate.
static void irq_unregister(uint8_t irq) {
    if (irq >= 16) return;
    g_irq_handlers[irq] = nullptr;

    uint16_t port = (irq < 8u) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (irq < 8u) ? irq : (uint8_t)(irq - 8u);

    // Set the mask bit to disable the IRQ line.
    outb(port, (uint8_t)(inb(port) | (1u << bit)));
}

// ── init_irq ──────────────────────────────────────────────────────────────────
// Remap the 8259A PIC and mask every IRQ line.
//
// The ICW (Initialisation Command Word) sequence is defined by the 8259A spec:
//   ICW1 — start initialisation; edge-triggered; cascade; ICW4 required.
//   ICW2 — set vector offset for each PIC.
//   ICW3 — describe the cascade wiring (PIC1: bit mask; PIC2: cascade ID).
//   ICW4 — set 8086/88 mode.
//
// After init_irq() every IRQ line is masked.  Drivers unmask their own line
// by calling irq_register().
static void init_irq() {
    // Save existing Interrupt Mask Registers so we can re-apply them later
    // if needed (standard Linux technique; we mask all below anyway).
    // uint8_t saved1 = inb(PIC1_DATA);
    // uint8_t saved2 = inb(PIC2_DATA);

    // ── ICW1: begin initialisation ────────────────────────────────────────────
    outb(PIC1_CMD,  0x11); io_wait();
    outb(PIC2_CMD,  0x11); io_wait();

    // ── ICW2: vector offsets ──────────────────────────────────────────────────
    outb(PIC1_DATA, IRQ_BASE);   io_wait();   // IRQ 0-7  → vectors 32-39
    outb(PIC2_DATA, IRQ_BASE_S); io_wait();   // IRQ 8-15 → vectors 40-47

    // ── ICW3: cascade wiring ──────────────────────────────────────────────────
    // PIC1: bit 2 set = slave PIC is connected to IRQ2.
    // PIC2: cascade identity = 2 (binary, not a bit mask).
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();

    // ── ICW4: 8086/88 mode ────────────────────────────────────────────────────
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();

    // ── Mask all IRQ lines ────────────────────────────────────────────────────
    // Individual lines are unmasked when drivers call irq_register().
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    print((char*)"IRQ: PIC remapped (IRQ0-7 -> v32-39, IRQ8-15 -> v40-47). All lines masked.");
}

#endif // irq_h