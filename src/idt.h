#ifndef idt_h
#define idt_h

// ── idt.h ─────────────────────────────────────────────────────────────────────
// Sets up the 64-bit IDT using explicit naked assembly stubs.
//
// WHY NAKED STUBS INSTEAD OF __attribute__((interrupt)):
//   Clang's __attribute__((interrupt)) on x86-64 is unreliable for bare-metal
//   kernels — it can mishandle the error-code ABI, misalign the stack before
//   calling nested functions, or generate a prologue that touches the red zone
//   before the caller's frame is fully saved.  The naked-stub pattern is what
//   Linux, FreeBSD, and every serious x86 kernel uses: tiny per-vector asm
//   stubs push a dummy error code (if needed) and the vector number, then jump
//   to a single shared C-callable handler that saves all GPRs, calls C++, and
//   iretqs.  The compiler never touches the interrupt stack.
//
// STACK LAYOUT when exception_dispatch() is called:
//   [rsp+  0]  r15          <- lowest address (last pushed by common stub)
//   [rsp+  8]  r14
//   [rsp+ 16]  r13
//   [rsp+ 24]  r12
//   [rsp+ 32]  r11
//   [rsp+ 40]  r10
//   [rsp+ 48]  r9
//   [rsp+ 56]  r8
//   [rsp+ 64]  rdi
//   [rsp+ 72]  rsi
//   [rsp+ 80]  rbp
//   [rsp+ 88]  rbx
//   [rsp+ 96]  rdx
//   [rsp+104]  rcx
//   [rsp+112]  rax
//   [rsp+120]  vector        <- pushed by ISR stub
//   [rsp+128]  error_code    <- pushed by CPU (or 0 by stub if no error code)
//   [rsp+136]  rip           <- pushed by CPU
//   [rsp+144]  cs
//   [rsp+152]  rflags
//   [rsp+160]  rsp (old)
//   [rsp+168]  ss
//
// IRQ HANDLING:
//   Vectors 32-47 are hardware IRQ lines (PIC-remapped IRQ 0-15).
//   exception_dispatch() routes these through g_irq_handlers[] and sends EOI
//   automatically, so registered handlers never touch the PIC directly.
//   irq.h provides irq_register() which installs handlers and unmasks lines.
//
// Call init_idt() after init_gdt().
// ──────────────────────────────────────────────────────────────────────────────

#include <stdint.h>
#include "helpers.h"   // print(), to_hex(), hcf()

// ── Gate descriptor ───────────────────────────────────────────────────────────
struct IDTEntry {
    uint16_t offset_low;
    uint16_t selector;    // always SEG_KERNEL_CODE = 0x08
    uint8_t  ist;         // IST index (0 = use current RSP; 1 = IST1 from TSS)
    uint8_t  type_attr;   // P | DPL | 0 | gate-type
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed, aligned(16)));

struct IDTPointer {
    uint16_t size;
    uint64_t offset;
} __attribute__((packed));

#define IDT_GATE_INTERRUPT  0x8E   // clears IF on entry
#define IDT_GATE_TRAP       0x8F   // leaves IF unchanged

// ── Full register frame as laid out on the stack ──────────────────────────────
// Passed by pointer to exception_dispatch().
struct ExceptionFrame {
    // Saved by isr_common_stub (lowest address first = last pushed):
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    // Pushed by ISR stub:
    uint64_t vector;
    uint64_t error_code;   // 0 for vectors without a CPU-pushed error code
    // Pushed by CPU on exception entry:
    uint64_t rip, cs, rflags, rsp, ss;
};

// ── IRQ handler table ─────────────────────────────────────────────────────────
// irq.h populates this table via irq_register().
// exception_dispatch() calls g_irq_handlers[irq] when vector is 32-47.
// Handlers must NOT send EOI — exception_dispatch() does that automatically.
typedef void (*irq_handler_t)();
static irq_handler_t g_irq_handlers[16] = {};

// ── Minimal PIC EOI helper ────────────────────────────────────────────────────
// Used only by exception_dispatch().  The full port I/O API (outb/inb) lives
// in irq.h to avoid a circular-include dependency.
//
// Send non-specific EOI to PIC1 (and PIC2 if the IRQ came from the slave).
// IRQ  0-7 : master PIC1 only.
// IRQ 8-15 : slave PIC2 first, then master PIC1 (both must be acknowledged).
static inline void _pic_eoi(uint8_t irq) {
    if (irq >= 8u) {
        // EOI to slave PIC2  (port 0xA0)
        __asm__ volatile (
            "outb %0, %1"
            :: "a"((uint8_t)0x20u), "Nd"((uint16_t)0xA0u)
            : "memory"
        );
    }
    // EOI to master PIC1  (port 0x20)
    __asm__ volatile (
        "outb %0, %1"
        :: "a"((uint8_t)0x20u), "Nd"((uint16_t)0x20u)
        : "memory"
    );
}

// ── Module globals ────────────────────────────────────────────────────────────
alignas(16) static IDTEntry g_idt[256];
static IDTPointer            g_idt_ptr;

static void idt_set_gate(uint8_t vector, void* handler,
                          uint8_t ist, uint8_t type_attr) {
    uint64_t addr = (uint64_t)handler;
    g_idt[vector].offset_low  = (uint16_t)(addr         & 0xFFFF);
    g_idt[vector].selector    = 0x08;   // SEG_KERNEL_CODE
    g_idt[vector].ist         = ist;
    g_idt[vector].type_attr   = type_attr;
    g_idt[vector].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    g_idt[vector].offset_high = (uint32_t)(addr  >> 32);
    g_idt[vector].reserved    = 0;
}

// ── kpanic ────────────────────────────────────────────────────────────────────
// Displays the exception name and all saved registers, then halts permanently.
// Never returns.
static __attribute__((noinline))
void kpanic(const char* name, ExceptionFrame* f) {
    char buf[32];

    print((char*)"");
    print((char*)"==============================");
    print((char*)"     *** KERNEL PANIC ***     ");
    print((char*)"==============================");
    print((char*)name);
    print((char*)"------------------------------");

    print((char*)"RIP:    "); to_hex(f->rip,        buf); print(buf);
    print((char*)"CS:     "); to_hex(f->cs,          buf); print(buf);
    print((char*)"RFLAGS: "); to_hex(f->rflags,      buf); print(buf);
    print((char*)"RSP:    "); to_hex(f->rsp,         buf); print(buf);
    print((char*)"SS:     "); to_hex(f->ss,          buf); print(buf);
    print((char*)"ERR:    "); to_hex(f->error_code,  buf); print(buf);
    print((char*)"VEC:    "); to_hex(f->vector,      buf); print(buf);
    print((char*)"RAX:    "); to_hex(f->rax,         buf); print(buf);
    print((char*)"RBX:    "); to_hex(f->rbx,         buf); print(buf);
    print((char*)"RCX:    "); to_hex(f->rcx,         buf); print(buf);
    print((char*)"RDX:    "); to_hex(f->rdx,         buf); print(buf);

    // CR2 holds the faulting virtual address for page faults.
    uint64_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    print((char*)"CR2:    "); to_hex(cr2, buf); print(buf);

    if (f->vector == 14) {
        uint64_t err = f->error_code;
        print((char*)"PF ERR: [P="); if (err & (1u << 0)) print((char*)"1"); else print((char*)"0");
        print((char*)", W/R=");      if (err & (1u << 1)) print((char*)"W"); else print((char*)"R");
        print((char*)", U/S=");      if (err & (1u << 2)) print((char*)"U"); else print((char*)"S");
        print((char*)", RSVD=");     if (err & (1u << 3)) print((char*)"1"); else print((char*)"0");
        print((char*)", I/D=");      if (err & (1u << 4)) print((char*)"I"); else print((char*)"D");
        print((char*)"]");
    }

    print((char*)"------------------------------");
    print((char*)"System halted.");
    hcf();
}

// ── Exception name table ──────────────────────────────────────────────────────
static const char* const exception_names[32] = {
    "Exception #0:  Divide-by-Zero",
    "Exception #1:  Debug",
    "Exception #2:  Non-Maskable Interrupt",
    "Exception #3:  Breakpoint (INT3)",
    "Exception #4:  Overflow (INTO)",
    "Exception #5:  BOUND Range Exceeded",
    "Exception #6:  Invalid Opcode (UD2)",
    "Exception #7:  Device Not Available (FPU/SSE)",
    "Exception #8:  *** DOUBLE FAULT ***",
    "Exception #9:  Coprocessor Segment Overrun",
    "Exception #10: Invalid TSS",
    "Exception #11: Segment Not Present",
    "Exception #12: Stack-Segment Fault",
    "Exception #13: General Protection Fault",
    "Exception #14: Page Fault",
    "Exception #15: Reserved",
    "Exception #16: x87 Floating-Point Error",
    "Exception #17: Alignment Check",
    "Exception #18: Machine Check",
    "Exception #19: SIMD Floating-Point Exception",
    "Exception #20: Virtualization Exception",
    "Exception #21: Control-Protection Exception (CET)",
    "Exception #22: Reserved",
    "Exception #23: Reserved",
    "Exception #24: Reserved",
    "Exception #25: Reserved",
    "Exception #26: Reserved",
    "Exception #27: Reserved",
    "Exception #28: Hypervisor Injection",
    "Exception #29: VMM Communication Exception",
    "Exception #30: Security Exception",
    "Exception #31: Reserved",
};

// ── exception_dispatch ────────────────────────────────────────────────────────
// Called from isr_common_stub with a pointer to the full register frame.
// extern "C" so the naked-asm stub can reference it by its unmangled name.
//
// ROUTING:
//   vectors  0-31 : CPU exceptions → kpanic (never returns)
//   vectors 32-47 : hardware IRQs  → call g_irq_handlers[irq], send EOI, return
//   vectors 48+   : unregistered   → kpanic (catches stray APIC/MSI vectors
//                                    that arrive before we handle them)
extern "C" __attribute__((used))
void exception_dispatch(ExceptionFrame* f) {

    // ── Hardware IRQ path (vectors 32-47, PIC IRQ lines 0-15) ─────────────────
    //
    // After init_irq() remaps the PIC:
    //   vector 32  = IRQ 0  (PIT timer)
    //   vector 33  = IRQ 1  (PS/2 keyboard — not used; we use XHCI)
    //   vector 40  = IRQ 8  (RTC)
    //   ...
    //
    // We call the registered handler (if any), then send EOI so the PIC
    // deasserts the interrupt line and can fire again.  If no handler is
    // registered the interrupt is silently acknowledged — this prevents the
    // spurious interrupt from becoming a kernel panic while we're still wiring
    // things up.
    if (f->vector >= 32u && f->vector < 48u) {
        uint8_t irq = (uint8_t)(f->vector - 32u);
        if (g_irq_handlers[irq]) {
            g_irq_handlers[irq]();
        }
        _pic_eoi(irq);
        return;   // isr_common_stub will iretq
    }

    // ── CPU exceptions (0-31) and unknown vectors (48+): kernel panic ─────────
    const char* name = (f->vector < 32u)
                       ? exception_names[f->vector]
                       : "Unhandled Interrupt (vector >= 48 — no handler registered)";
    kpanic(name, f);
}

// ── isr_common_stub ───────────────────────────────────────────────────────────
// All per-vector stubs jump here.
// On entry: [rsp] = vector, [rsp+8] = error_code (real or 0), then CPU frame.
// We push all GPRs, align the stack, call exception_dispatch, then unwind.
__attribute__((naked)) extern "C"
void isr_common_stub() {
    __asm__ volatile (
        // ── Save all general-purpose registers ────────────────────────────────
        "pushq %%rax\n\t"
        "pushq %%rcx\n\t"
        "pushq %%rdx\n\t"
        "pushq %%rbx\n\t"
        "pushq %%rbp\n\t"
        "pushq %%rsi\n\t"
        "pushq %%rdi\n\t"
        "pushq %%r8\n\t"
        "pushq %%r9\n\t"
        "pushq %%r10\n\t"
        "pushq %%r11\n\t"
        "pushq %%r12\n\t"
        "pushq %%r13\n\t"
        "pushq %%r14\n\t"
        "pushq %%r15\n\t"

        // ── Call exception_dispatch(ExceptionFrame* f) ────────────────────────
        "movq %%rsp, %%rdi\n\t"

        // Save true rsp in rbx before aligning.
        // rbx is callee-saved, so exception_dispatch() will not clobber it.
        "movq %%rsp, %%rbx\n\t"

        // Align rsp to 16 bytes (System V ABI requirement before any call).
        "andq $-16, %%rsp\n\t"

        "callq exception_dispatch\n\t"

        // ── Restore rsp and registers ─────────────────────────────────────────
        "movq %%rbx, %%rsp\n\t"

        "popq %%r15\n\t"
        "popq %%r14\n\t"
        "popq %%r13\n\t"
        "popq %%r12\n\t"
        "popq %%r11\n\t"
        "popq %%r10\n\t"
        "popq %%r9\n\t"
        "popq %%r8\n\t"
        "popq %%rdi\n\t"
        "popq %%rsi\n\t"
        "popq %%rbp\n\t"
        "popq %%rbx\n\t"
        "popq %%rdx\n\t"
        "popq %%rcx\n\t"
        "popq %%rax\n\t"

        // Discard vector number and error code pushed by the ISR stub.
        "addq $16, %%rsp\n\t"

        "iretq\n\t"
        ::: "memory"
    );
}

// ── ISR stub macros ───────────────────────────────────────────────────────────
#define ISR_NO_ERR(n)                                         \
    __attribute__((naked)) static void isr_stub_##n() {       \
        __asm__ volatile (                                    \
            "pushq $0\n\t"           /* dummy error code */   \
            "pushq $" #n "\n\t"      /* vector number    */   \
            "jmp isr_common_stub\n\t"                         \
        );                                                    \
    }

#define ISR_ERR(n)                                            \
    __attribute__((naked)) static void isr_stub_##n() {       \
        __asm__ volatile (                                    \
            "pushq $" #n "\n\t"      /* vector number */      \
            "jmp isr_common_stub\n\t"                         \
        );                                                    \
    }

// ── CPU exception stubs (vectors 0-31) ───────────────────────────────────────
ISR_NO_ERR(0)    // #DE  Divide-by-Zero
ISR_NO_ERR(1)    // #DB  Debug
ISR_NO_ERR(2)    //      NMI
ISR_NO_ERR(3)    // #BP  Breakpoint
ISR_NO_ERR(4)    // #OF  Overflow
ISR_NO_ERR(5)    // #BR  BOUND Range Exceeded
ISR_NO_ERR(6)    // #UD  Invalid Opcode
ISR_NO_ERR(7)    // #NM  Device Not Available
ISR_ERR(8)       // #DF  Double Fault          (error code always 0)
ISR_NO_ERR(9)    //      Coprocessor Segment Overrun
ISR_ERR(10)      // #TS  Invalid TSS
ISR_ERR(11)      // #NP  Segment Not Present
ISR_ERR(12)      // #SS  Stack-Segment Fault
ISR_ERR(13)      // #GP  General Protection Fault
ISR_ERR(14)      // #PF  Page Fault
ISR_NO_ERR(15)   //      Reserved
ISR_NO_ERR(16)   // #MF  x87 FPU Error
ISR_ERR(17)      // #AC  Alignment Check
ISR_NO_ERR(18)   // #MC  Machine Check
ISR_NO_ERR(19)   // #XM  SIMD FP Exception
ISR_NO_ERR(20)   // #VE  Virtualization Exception
ISR_ERR(21)      // #CP  Control-Protection (CET)
ISR_NO_ERR(22)   //      Reserved
ISR_NO_ERR(23)   //      Reserved
ISR_NO_ERR(24)   //      Reserved
ISR_NO_ERR(25)   //      Reserved
ISR_NO_ERR(26)   //      Reserved
ISR_NO_ERR(27)   //      Reserved
ISR_NO_ERR(28)   // #HV  Hypervisor Injection
ISR_NO_ERR(29)   // #VC  VMM Communication
ISR_ERR(30)      // #SX  Security Exception
ISR_NO_ERR(31)   //      Reserved

// ── Hardware IRQ stubs (vectors 32-47, PIC IRQ lines 0-15) ───────────────────
// Each stub must push its OWN vector number so exception_dispatch() can
// compute the correct IRQ line (vector - 32) and send EOI to the right PIC.
// Using a single shared stub (as was done before) would push 32 for every IRQ,
// making IRQs 1-15 indistinguishable from IRQ 0 and sending EOI to PIC1 always,
// which would leave PIC2 (IRQ 8-15) permanently locked.
ISR_NO_ERR(32)   // IRQ 0  — PIT timer channel 0
ISR_NO_ERR(33)   // IRQ 1  — PS/2 keyboard (not used; xHCI handles HID)
ISR_NO_ERR(34)   // IRQ 2  — cascade (PIC2); should never fire as an interrupt
ISR_NO_ERR(35)   // IRQ 3  — COM2 serial
ISR_NO_ERR(36)   // IRQ 4  — COM1 serial
ISR_NO_ERR(37)   // IRQ 5  — LPT2 / sound card
ISR_NO_ERR(38)   // IRQ 6  — floppy disk controller
ISR_NO_ERR(39)   // IRQ 7  — LPT1 / spurious PIC1 (check ISR before handling)
ISR_NO_ERR(40)   // IRQ 8  — RTC
ISR_NO_ERR(41)   // IRQ 9  — ACPI / PCI redirect
ISR_NO_ERR(42)   // IRQ 10 — available / PCI
ISR_NO_ERR(43)   // IRQ 11 — available / PCI
ISR_NO_ERR(44)   // IRQ 12 — PS/2 mouse
ISR_NO_ERR(45)   // IRQ 13 — x87 FPU / coprocessor
ISR_NO_ERR(46)   // IRQ 14 — ATA primary
ISR_NO_ERR(47)   // IRQ 15 — ATA secondary / spurious PIC2

// Generic placeholder for unhandled vectors 48-255.
// Will be replaced with APIC/MSI handlers in the APIC phase.
ISR_NO_ERR(48)

// ── init_idt ──────────────────────────────────────────────────────────────────
static void init_idt() {

    // Seed all 256 entries with the generic unhandled stub.
    // Stray vectors produce a panic rather than a triple fault.
    for (int i = 0; i < 256; i++)
        idt_set_gate((uint8_t)i, (void*)isr_stub_48, 0, IDT_GATE_INTERRUPT);

    // ── CPU exceptions 0-31 ───────────────────────────────────────────────────
    idt_set_gate(0,  (void*)isr_stub_0,  0, IDT_GATE_INTERRUPT);
    idt_set_gate(1,  (void*)isr_stub_1,  0, IDT_GATE_TRAP);       // #DB: trap gate
    idt_set_gate(2,  (void*)isr_stub_2,  0, IDT_GATE_INTERRUPT);
    idt_set_gate(3,  (void*)isr_stub_3,  0, IDT_GATE_TRAP);       // #BP: trap gate
    idt_set_gate(4,  (void*)isr_stub_4,  0, IDT_GATE_INTERRUPT);
    idt_set_gate(5,  (void*)isr_stub_5,  0, IDT_GATE_INTERRUPT);
    idt_set_gate(6,  (void*)isr_stub_6,  0, IDT_GATE_INTERRUPT);
    idt_set_gate(7,  (void*)isr_stub_7,  0, IDT_GATE_INTERRUPT);
    // #DF uses IST=1 so the CPU loads the dedicated df_stack from gdt.h
    idt_set_gate(8,  (void*)isr_stub_8,  1, IDT_GATE_INTERRUPT);
    idt_set_gate(9,  (void*)isr_stub_9,  0, IDT_GATE_INTERRUPT);
    idt_set_gate(10, (void*)isr_stub_10, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(11, (void*)isr_stub_11, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(12, (void*)isr_stub_12, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(13, (void*)isr_stub_13, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(14, (void*)isr_stub_14, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(15, (void*)isr_stub_15, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(16, (void*)isr_stub_16, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(17, (void*)isr_stub_17, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(18, (void*)isr_stub_18, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(19, (void*)isr_stub_19, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(20, (void*)isr_stub_20, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(21, (void*)isr_stub_21, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(22, (void*)isr_stub_22, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(23, (void*)isr_stub_23, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(24, (void*)isr_stub_24, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(25, (void*)isr_stub_25, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(26, (void*)isr_stub_26, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(27, (void*)isr_stub_27, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(28, (void*)isr_stub_28, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(29, (void*)isr_stub_29, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(30, (void*)isr_stub_30, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(31, (void*)isr_stub_31, 0, IDT_GATE_INTERRUPT);

    // ── Hardware IRQ vectors 32-47 (PIC IRQ lines 0-15) ──────────────────────
    // Each vector gets its own stub so the vector number pushed on the stack
    // is correct, allowing exception_dispatch() to compute irq = vector - 32
    // and send EOI to the right PIC (master for 0-7, master+slave for 8-15).
    idt_set_gate(32, (void*)isr_stub_32, 0, IDT_GATE_INTERRUPT);  // IRQ 0  PIT
    idt_set_gate(33, (void*)isr_stub_33, 0, IDT_GATE_INTERRUPT);  // IRQ 1  PS/2 kbd
    idt_set_gate(34, (void*)isr_stub_34, 0, IDT_GATE_INTERRUPT);  // IRQ 2  cascade
    idt_set_gate(35, (void*)isr_stub_35, 0, IDT_GATE_INTERRUPT);  // IRQ 3  COM2
    idt_set_gate(36, (void*)isr_stub_36, 0, IDT_GATE_INTERRUPT);  // IRQ 4  COM1
    idt_set_gate(37, (void*)isr_stub_37, 0, IDT_GATE_INTERRUPT);  // IRQ 5  LPT2
    idt_set_gate(38, (void*)isr_stub_38, 0, IDT_GATE_INTERRUPT);  // IRQ 6  floppy
    idt_set_gate(39, (void*)isr_stub_39, 0, IDT_GATE_INTERRUPT);  // IRQ 7  LPT1
    idt_set_gate(40, (void*)isr_stub_40, 0, IDT_GATE_INTERRUPT);  // IRQ 8  RTC
    idt_set_gate(41, (void*)isr_stub_41, 0, IDT_GATE_INTERRUPT);  // IRQ 9  ACPI
    idt_set_gate(42, (void*)isr_stub_42, 0, IDT_GATE_INTERRUPT);  // IRQ 10 PCI
    idt_set_gate(43, (void*)isr_stub_43, 0, IDT_GATE_INTERRUPT);  // IRQ 11 PCI
    idt_set_gate(44, (void*)isr_stub_44, 0, IDT_GATE_INTERRUPT);  // IRQ 12 PS/2 mouse
    idt_set_gate(45, (void*)isr_stub_45, 0, IDT_GATE_INTERRUPT);  // IRQ 13 FPU
    idt_set_gate(46, (void*)isr_stub_46, 0, IDT_GATE_INTERRUPT);  // IRQ 14 ATA pri
    idt_set_gate(47, (void*)isr_stub_47, 0, IDT_GATE_INTERRUPT);  // IRQ 15 ATA sec

    // ── Load IDTR ─────────────────────────────────────────────────────────────
    g_idt_ptr.size   = (uint16_t)(sizeof(g_idt) - 1);
    g_idt_ptr.offset = (uint64_t)&g_idt;

    __asm__ volatile ("lidt %0" :: "m"(g_idt_ptr) : "memory");

    print((char*)"IDT loaded -- exceptions armed, IRQ vectors 32-47 ready.");
}

#endif // idt_h 