#ifndef gdt_h
#define gdt_h

// ── gdt.h ─────────────────────────────────────────────────────────────────────
// Sets up a minimal 64-bit GDT with:
//   Ring-0 code + data  (kernel)
//   Ring-3 code + data  (user — ready for later, not used yet)
//   64-bit TSS          (needed for IST stacks and, later, syscall RSP0)
//
// Call init_gdt() once, near the top of kmain, before init_idt().
// ──────────────────────────────────────────────────────────────────────────────

#include <stdint.h>

// ── Segment selectors ─────────────────────────────────────────────────────────
// Byte offset into the GDT = entry index × 8.
// User selectors are OR'd with 3 (RPL=3) when used for ring-3 transitions.
#define SEG_KERNEL_CODE  0x08
#define SEG_KERNEL_DATA  0x10
#define SEG_USER_DATA    0x18   // SYSRET requires: user data immediately before user code
#define SEG_USER_CODE    0x20
#define SEG_TSS          0x28   // Two consecutive 8-byte slots = 16-byte system descriptor

// ── Descriptor structures ─────────────────────────────────────────────────────

// Standard 8-byte code/data segment descriptor.
struct GDTEntry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;            // P | DPL | S | Type
    uint8_t  flags_limit_high;  // G | D/B | L | AVL | Limit[19:16]
    uint8_t  base_high;
} __attribute__((packed));

// 16-byte system descriptor used for the TSS in 64-bit mode.
// Occupies two consecutive GDT slots — fits naturally in GDTTable below.
struct SystemDescriptor {
    uint16_t limit_low;
    uint16_t base_0_15;
    uint8_t  base_16_23;
    uint8_t  access;            // P=1 DPL=0 S=0 Type=0x9 (64-bit TSS Available)
    uint8_t  flags_limit_high;
    uint8_t  base_24_31;
    uint32_t base_32_63;
    uint32_t reserved;
} __attribute__((packed));

// GDTR operand for the lgdt instruction.
struct GDTPointer {
    uint16_t size;    // sizeof(GDTTable) - 1
    uint64_t offset;  // linear address of GDT
} __attribute__((packed));

// Task State Segment — 64-bit layout (Intel SDM Vol.3 §7.7).
// We only need IST1 (for double-fault) and RSP0 (kernel stack for syscalls later).
struct TSS {
    uint32_t reserved0;
    uint64_t rsp[3];        // RSP0–RSP2 (privilege-level stacks)
    uint64_t reserved1;
    uint64_t ist[7];        // IST1–IST7  (ist[0] = IST1)
    uint8_t  reserved2[10];
    uint16_t iopb_offset;   // offset to I/O permission bitmap; set to sizeof(TSS) = none
} __attribute__((packed));

// Complete GDT — a single flat struct so &g_gdt is the GDT base address.
struct GDTTable {
    GDTEntry       null_desc;    // [0x00] required-zero null descriptor
    GDTEntry       kernel_code;  // [0x08]
    GDTEntry       kernel_data;  // [0x10]
    GDTEntry       user_data;    // [0x18]
    GDTEntry       user_code;    // [0x20]
    SystemDescriptor tss_desc;   // [0x28]  two slots wide
} __attribute__((packed, aligned(8)));

// ── Module globals ─────────────────────────────────────────────────────────────
static GDTTable   g_gdt;
static GDTPointer g_gdt_ptr;
static TSS        g_tss;

// Dedicated 32 KiB stack for the double-fault IST1 entry.
// Using IST for #DF means the CPU switches to this stack regardless of RSP,
// so even a corrupted/overflowed kernel stack won't cause a triple fault.
alignas(16) static uint8_t g_df_stack[32768];

// ── Internal helpers ──────────────────────────────────────────────────────────
static inline void gdt_write_entry(GDTEntry& e,
                                    uint32_t base, uint32_t limit,
                                    uint8_t access, uint8_t flags) {
    e.limit_low        = (uint16_t)(limit & 0xFFFF);
    e.base_low         = (uint16_t)(base  & 0xFFFF);
    e.base_mid         = (uint8_t)((base  >> 16) & 0xFF);
    e.access           = access;
    // flags occupies the upper nibble; limit[19:16] occupies the lower nibble.
    e.flags_limit_high = (uint8_t)((flags & 0xF0) | ((limit >> 16) & 0x0F));
    e.base_high        = (uint8_t)((base  >> 24) & 0xFF);
}

// ── init_gdt ──────────────────────────────────────────────────────────────────
static void init_gdt() {

    // ── Null descriptor (required to be all-zero) ─────────────────────────────
    gdt_write_entry(g_gdt.null_desc, 0, 0, 0, 0);

    // ── Kernel code (ring 0, 64-bit) ──────────────────────────────────────────
    // Access 0x9A = 1001'1010:  P=1  DPL=0  S=1  Type=0xA (execute/read)
    // Flags  0xA0 = 1010'xxxx:  G=1  D=0    L=1  (L=1 → 64-bit code segment)
    gdt_write_entry(g_gdt.kernel_code, 0, 0xFFFFF, 0x9A, 0xA0);

    // ── Kernel data (ring 0) ─────────────────────────────────────────────────
    // Access 0x92 = 1001'0010:  P=1  DPL=0  S=1  Type=0x2 (read/write)
    // Flags  0xC0 = 1100'xxxx:  G=1  D=1    L=0
    gdt_write_entry(g_gdt.kernel_data, 0, 0xFFFFF, 0x92, 0xC0);

    // ── User data (ring 3) ────────────────────────────────────────────────────
    // Access 0xF2 = 1111'0010:  P=1  DPL=3  S=1  Type=0x2
    // Flags  0xC0                G=1  D=1    L=0
    // NOTE: SYSRET expects the user data descriptor to sit immediately before
    // the user code descriptor, which is why user_data comes before user_code.
    gdt_write_entry(g_gdt.user_data, 0, 0xFFFFF, 0xF2, 0xC0);

    // ── User code (ring 3, 64-bit) ────────────────────────────────────────────
    // Access 0xFA = 1111'1010:  P=1  DPL=3  S=1  Type=0xA
    // Flags  0xA0                G=1  D=0    L=1  (64-bit)
    gdt_write_entry(g_gdt.user_code, 0, 0xFFFFF, 0xFA, 0xA0);

    // ── TSS ───────────────────────────────────────────────────────────────────
    g_tss.iopb_offset = sizeof(TSS);  // no I/O permission bitmap entries
    // IST1 = top of df_stack (stacks grow down, so +sizeof gives the initial RSP).
    g_tss.ist[0] = (uint64_t)(g_df_stack + sizeof(g_df_stack));
    // rsp[0] will be set to the kernel stack pointer when we add syscalls;
    // it's unused for now (we never drop to ring 3 yet).

    uint64_t tss_base  = (uint64_t)&g_tss;
    uint32_t tss_limit = (uint32_t)(sizeof(TSS) - 1);

    // 16-byte TSS descriptor (S=0 → system descriptor, Type=9 → 64-bit TSS).
    // Access 0x89 = 1000'1001: P=1  DPL=0  S=0  Type=9
    g_gdt.tss_desc.limit_low        = (uint16_t)(tss_limit & 0xFFFF);
    g_gdt.tss_desc.base_0_15        = (uint16_t)(tss_base  & 0xFFFF);
    g_gdt.tss_desc.base_16_23       = (uint8_t)((tss_base  >> 16) & 0xFF);
    g_gdt.tss_desc.access           = 0x89;
    g_gdt.tss_desc.flags_limit_high = (uint8_t)((tss_limit >> 16) & 0x0F);
    g_gdt.tss_desc.base_24_31       = (uint8_t)((tss_base  >> 24) & 0xFF);
    g_gdt.tss_desc.base_32_63       = (uint32_t)(tss_base  >> 32);
    g_gdt.tss_desc.reserved         = 0;

    // ── Load GDT + reload segment registers ──────────────────────────────────
    g_gdt_ptr.size   = sizeof(GDTTable) - 1;
    g_gdt_ptr.offset = (uint64_t)&g_gdt;

    __asm__ volatile (
        // 1. Load the new GDTR.
        "lgdt %[gdtr]\n\t"

        // 2. Reload CS (code segment register).
        //    In 64-bit mode there is no ljmp-with-immediate that can encode a
        //    64-bit target, so the standard technique is:
        //      push <new CS>
        //      push <return RIP>
        //      lretq               ← pops RIP then CS, does a far return
        "pushq $0x08\n\t"               // SEG_KERNEL_CODE
        "leaq  1f(%%rip), %%rax\n\t"   // address of label "1" below
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"

        // 3. Reload all data-segment registers to the new kernel data selector.
        "movw $0x10, %%ax\n\t"   // SEG_KERNEL_DATA
        "movw %%ax,  %%ds\n\t"
        "movw %%ax,  %%es\n\t"
        "movw %%ax,  %%ss\n\t"
        // FS and GS are null for now; they will hold per-CPU pointers later.
        "xorw %%ax,  %%ax\n\t"
        "movw %%ax,  %%fs\n\t"
        "movw %%ax,  %%gs\n\t"

        // 4. Load the TSS so the CPU knows where IST stacks live.
        "movw $0x28, %%ax\n\t"   // SEG_TSS
        "ltr  %%ax\n\t"

        : /* no outputs */
        : [gdtr] "m"(g_gdt_ptr)
        : "rax", "memory"
    );
}

#endif // gdt_h