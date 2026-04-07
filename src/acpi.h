#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>
#include <stddef.h>
#include "helpers.h"
#include "structures.h"
#include "tty.h"

extern "C" {
    #include "../limine.h"
}

// Ensure the linker can find requests defined in kernel.cpp
extern volatile struct limine_rsdp_request rsdp_req;

struct RSDPDescriptor {
    char Signature[8];
    uint8_t Checksum;
    char OEMID[6];
    uint8_t Revision;
    uint32_t RsdtAddress;
} __attribute__ ((packed));

struct RSDPDescriptor20 {
    struct RSDPDescriptor firstPart;
    uint32_t Length;
    uint64_t XsdtAddress;
    uint8_t ExtendedChecksum;
    uint8_t reserved[3];
} __attribute__ ((packed));

// Generic Address Structure (ACPI 2.0+, §5.2.3.2)
struct GAS {
    uint8_t  address_space;  // 0=memory, 1=I/O port, 2=PCI config
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
} __attribute__ ((packed));

struct FADT {
    struct ACPISDTHeader h;
    uint32_t FirmwareCtrl;
    uint32_t Dsdt;
    uint8_t  Reserved;
    uint8_t  PreferredProfile;
    uint16_t SCI_Interrupt;
    uint32_t SMI_CommandPort;
    uint8_t  AcpiEnable;
    uint8_t  AcpiDisable;
    uint8_t  S4BIOS_REQ;
    uint8_t  PSTATE_CNT;
    uint32_t PM1aEventBlock;
    uint32_t PM1bEventBlock;
    uint32_t PM1aControlBlock;
    uint32_t PM1bControlBlock;
    uint32_t PM2ControlBlock;
    uint32_t PMTimerBlock;
    uint32_t GPE0Block;
    uint32_t GPE1Block;
    uint8_t  PM1EventLength;
    uint8_t  PM1ControlLength;
    uint8_t  PM2ControlLength;
    uint8_t  PMTimerLength;
    uint8_t  GPE0Length;        // total bytes: low half = status, high half = enable
    uint8_t  GPE1Length;
    uint8_t  GPE1Base;
    uint8_t  CSTControl;
    uint16_t C2Latency;
    uint16_t C3Latency;
    uint16_t FlushSize;
    uint16_t FlushStride;
    uint8_t  DutyOffset;
    uint8_t  DutyWidth;
    uint8_t  DayAlarm;
    uint8_t  MonthAlarm;
    uint8_t  Century;
    uint16_t BootArchFlags;
    uint8_t  Reserved2;
    uint32_t Flags;
    struct GAS ResetReg;        // ACPI 2.0+: register to write for reset
    uint8_t  ResetValue;        // value to write to ResetReg
} __attribute__ ((packed));

static inline void outw_acpi(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline void outb_acpi(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t inw_acpi(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// ── Find FADT ─────────────────────────────────────────────────────────────────
// Shared by both shutdown and reset so we don't duplicate the RSDP walk.
static struct FADT* find_fadt(void) {
    if (!rsdp_req.response || rsdp_req.response->address == nullptr)
        return nullptr;

    struct RSDPDescriptor20* rsdp = (struct RSDPDescriptor20*)rsdp_req.response->address;

    uint8_t* table_pointers;
    int entries;
    int pointer_size;

    if (rsdp->firstPart.Revision >= 2 && rsdp->XsdtAddress != 0) {
        struct ACPISDTHeader* xsdt = (struct ACPISDTHeader*)(rsdp->XsdtAddress + HHDM);
        entries = (xsdt->length - sizeof(struct ACPISDTHeader)) / 8;
        table_pointers = (uint8_t*)xsdt + sizeof(struct ACPISDTHeader);
        pointer_size = 8;
    } else {
        struct ACPISDTHeader* rsdt = (struct ACPISDTHeader*)((uint64_t)rsdp->firstPart.RsdtAddress + HHDM);
        entries = (rsdt->length - sizeof(struct ACPISDTHeader)) / 4;
        table_pointers = (uint8_t*)rsdt + sizeof(struct ACPISDTHeader);
        pointer_size = 4;
    }

    for (int i = 0; i < entries; i++) {
        uint64_t addr = 0;
        if (pointer_size == 8) addr = *(uint64_t*)(table_pointers + i * 8);
        else                   addr = *(uint32_t*)(table_pointers + i * 4);

        struct ACPISDTHeader* header = (struct ACPISDTHeader*)(addr + HHDM);
        if (strncmp(header->signature, "FACP", 4) == 0)
            return (struct FADT*)header;
    }
    return nullptr;
}

// ── hardware_reset ────────────────────────────────────────────────────────────
//
// Preferred path: ACPI FADT RESET_REG (ACPI 2.0+, §4.8.3.6).
// The FADT tells us exactly which register to write and what value to use.
// Address spaces we handle:
//   0 = SystemMemory  → MMIO byte write
//   1 = SystemIO      → outb  (the common case: e.g. port 0xB2, 0xCF9)
//   2 = PCI Config    → not implemented here; fall through to 0xCF9
//
// Fallback: port 0xCF9, value 0x06.
// This is the "PCI Reset" register present on virtually all x86 chipsets since
// the i440FX.  Bit 2 = FULL_RST (cold reset), bit 1 = SYS_RST (assert reset).
// Writing 0x06 triggers an immediate hard reset on any machine that doesn't
// respond to RESET_REG.
static void hardware_reset(void) {
    struct FADT* fadt = find_fadt();

    if (fadt) {
        // ACPI 2.0+ RESET_REG path.
        // The FADT Flags bit 10 (RESET_REG_SUP) indicates support, but in
        // practice checking that the address is non-zero is sufficient and
        // more compatible with buggy firmwares that forget to set the flag.
        if (fadt->h.revision >= 2 && fadt->ResetReg.address != 0) {
            uint8_t  val   = fadt->ResetValue;
            uint8_t  space = fadt->ResetReg.address_space;
            uint64_t addr  = fadt->ResetReg.address;

            if (space == 1) {
                // SystemIO — the overwhelmingly common case.
                outb_acpi((uint16_t)addr, val);
            } else if (space == 0) {
                // SystemMemory — write through HHDM mapping.
                *((volatile uint8_t*)(addr + HHDM)) = val;
            }
            // If neither worked (e.g. PCI config space), fall through to 0xCF9.
        }
    }

    // ── Fallback: port 0xCF9 hard reset ──────────────────────────────────────
    // Pulse the reset line via the chipset reset register.
    // First write 0x02 (de-assert, prepare for cold reset), then 0x06.
    // Some chipsets need the 0x02 write first; on others it is a no-op.
    outb_acpi(0xCF9, 0x02);
    for (int i = 0; i < 1000; i++) __asm__ volatile ("pause");
    outb_acpi(0xCF9, 0x06);

    // Should never reach here — park the CPU just in case.
    for (;;) __asm__ volatile ("hlt");
}

// ── hardware_shutdown ─────────────────────────────────────────────────────────
static void hardware_shutdown(void) {
    // We strictly use ACPI for shutdown because UEFI ResetSystem relies on
    // undocumented Limine memory translations that cause page faults on some firmwares.
    struct FADT* fadt = find_fadt();
    if (!fadt) return;

    // Enable ACPI mode by writing AcpiEnable to SMI_CommandPort.
    // We must wait until SCI_EN (bit 0 of PM1a_CNT) goes high before
    // writing SLP_EN.  On SMI-based firmware, writing SLP_EN while
    // SCI_EN=0 lets the SMI handler intercept the write and perform a
    // warm reset instead of a power-off.  Poll with a generous timeout
    // (~300 ms at ~1 GHz) to cover slow firmwares.
    if (fadt->SMI_CommandPort != 0 && fadt->AcpiEnable != 0) {
        if ((inw_acpi(fadt->PM1aControlBlock) & 1) == 0) {
            outb_acpi(fadt->SMI_CommandPort, fadt->AcpiEnable);
            for (int i = 0; i < 300000; i++) {
                if (inw_acpi(fadt->PM1aControlBlock) & 1) break;
                __asm__ volatile ("pause");
            }
        }
    }

    struct ACPISDTHeader* dsdt = (struct ACPISDTHeader*)((uint64_t)fadt->Dsdt + HHDM);
    if (!dsdt) return;

    uint8_t* aml = (uint8_t*)dsdt + sizeof(struct ACPISDTHeader);
    uint32_t aml_len = dsdt->length - sizeof(struct ACPISDTHeader);

    // STRICT SEARCH for: _ S 5 _ PackageOp(0x12)
    // We omit the NameOp (0x08) because ACPI can insert prefixes like \ (0x5C) or ^ before the name.
    // The name _S5_ will always be immediately followed by the PackageOp (0x12).
    uint8_t s5_sig[] = {'_', 'S', '5', '_', 0x12};
    uint8_t* s5 = nullptr;

    for (uint32_t i = 0; i < aml_len - sizeof(s5_sig); i++) {
        bool match = true;
        for (uint32_t j = 0; j < sizeof(s5_sig); j++) {
            if (aml[i + j] != s5_sig[j]) { match = false; break; }
        }
        if (match) { s5 = aml + i; break; }
    }

    // Default to SLP_TYP = 5, which is standard for most generic PC hardware (Award/AMI/Intel)
    // and correctly shuts them down instead of resetting.
    uint16_t slp_typa = 5;
    uint16_t slp_typb = 5;

    if (s5) {
        // We found it! s5 points to '_'. Jump to PackageOp.
        s5 += 4; // s5 now points to 0x12
        s5++;    // skip 0x12

        uint8_t pkg_len = *s5++;
        if ((pkg_len >> 6) > 0) {
            s5 += (pkg_len >> 6); // skip additional length bytes
        }

        s5++; // skip NumElements

        if (*s5 == 0x0A) { s5++; slp_typa = *s5++; } // BytePrefix
        else slp_typa = *s5++;

        if (*s5 == 0x0A) { s5++; slp_typb = *s5++; }
        else slp_typb = *s5++;
    } else {
        // If _S5_ not found, check for QEMU/BOCHS.
        // QEMU with SeaBIOS reports OEM IDs "BOCHS " or "BXPC  ".
        // Both use SLP_TYP=0 for S5.
        if (strncmp(fadt->h.oem_id, "BOCHS ", 6) == 0 ||
            strncmp(fadt->h.oem_id, "BXPC  ", 6) == 0) {
            slp_typa = 0;
            slp_typb = 0;
        }
    }

    // ── Clear all pending wake sources before asserting SLP_EN ───────────────
    //
    // If any PM1_STS or GPE_STS bit is set when SLP_EN fires, the ACPI
    // controller interprets it as an active wake event, enters S5, and
    // immediately wakes the system — the "turns off then back on" symptom.
    //
    // Order matters:
    //   1. Disable GPE enables first (write 0x00) so hardware can't re-assert
    //      a GPE status bit between the clear and the SLP_EN write.
    //   2. Clear GPE status (write 0xFF — writing 1s clears ACPI status bits).
    //   3. Clear PM1 event status.
    //   4. Write SLP_EN.

    // Step 1 & 2: GPE0 — disable enables, then clear status.
    // GPEx_BLK is GPExLength bytes total: low half = status regs, high half = enable regs.
    if (fadt->GPE0Block && fadt->GPE0Length) {
        uint8_t half = fadt->GPE0Length / 2;
        // Disable enables (second half of block)
        for (uint8_t i = 0; i < half; i++)
            outb_acpi((uint16_t)(fadt->GPE0Block + half + i), 0x00);
        // Clear status (first half of block)
        for (uint8_t i = 0; i < half; i++)
            outb_acpi((uint16_t)(fadt->GPE0Block + i), 0xFF);
    }

    // Step 1 & 2: GPE1 — same pattern.
    if (fadt->GPE1Block && fadt->GPE1Length) {
        uint8_t half = fadt->GPE1Length / 2;
        for (uint8_t i = 0; i < half; i++)
            outb_acpi((uint16_t)(fadt->GPE1Block + half + i), 0x00);
        for (uint8_t i = 0; i < half; i++)
            outb_acpi((uint16_t)(fadt->GPE1Block + i), 0xFF);
    }

    // Step 3: Clear PM1 fixed event status.
    // PM1x_EVT_BLK is PM1EventLength bytes total: low half = PM1x_STS, high half = PM1x_EN.
    // We only write to the status half (offset 0).
    if (fadt->PM1aEventBlock) outw_acpi((uint16_t)fadt->PM1aEventBlock, 0xFFFF);
    if (fadt->PM1bEventBlock) outw_acpi((uint16_t)fadt->PM1bEventBlock, 0xFFFF);

    // Step 4: Write SLP_TYP | SLP_EN to PM1_CNT.
    // Preserve the existing PM1a_CNT value (especially SCI_EN, bit 0).
    uint16_t pm1a_cur = inw_acpi(fadt->PM1aControlBlock);
    uint16_t val_a = (pm1a_cur & 0xE3FFu)           // clear old SLP_TYP/SLP_EN
                   | (uint16_t)(slp_typa << 10)
                   | (1u << 13);                     // set SLP_EN
    uint16_t val_b = (uint16_t)(slp_typb << 10) | (1u << 13);

    if (fadt->PM1aControlBlock) outw_acpi(fadt->PM1aControlBlock, val_a);
    if (fadt->PM1bControlBlock) outw_acpi(fadt->PM1bControlBlock, val_b);

    // Hardware should cut power within a few microseconds.
    // Spin forever so we never fall through into arbitrary code if the
    // firmware is slow to respond — without this, the CPU races ahead and
    // the system restarts instead of halting.
    for (;;) __asm__ volatile ("hlt");
}

#endif