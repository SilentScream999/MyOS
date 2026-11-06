#ifndef APIC_TIMER_H
#define APIC_TIMER_H

#include "timer.h"
#include "acpi.h"
#include "syscall.h"

// BSP-only fallback when firmware does not deliver legacy PIT IRQ0. No IOAPIC
// routing is needed for the CPU's own timer. Preserve firmware's x2APIC mode.
static bool g_timer_x2apic = false;
static volatile uint32_t* g_timer_lapic = nullptr;
static constexpr uint64_t TIMER_MMIO_BASE = 0xFFFF890000000000ULL;

static void timer_cpuid(uint32_t leaf, uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf), "c"(0));
}

static uint32_t timer_apic_read(uint32_t reg) {
    if (g_timer_x2apic) return (uint32_t)rdmsr(0x800 + (reg >> 4));
    return g_timer_lapic[reg / 4];
}

static void timer_apic_write(uint32_t reg, uint32_t value) {
    if (g_timer_x2apic) wrmsr(0x800 + (reg >> 4), value);
    else g_timer_lapic[reg / 4] = value;
}

static void local_timer_irq_handler() {
    timer_apic_write(0xB0, 0); // EOI before scheduler_tick can switch stacks
    g_tick_count++;
    scheduler_tick();
}

struct TimerHPETTable {
    ACPISDTHeader header;
    uint32_t block_id;
    GAS address;
    uint8_t number;
    uint16_t minimum_tick;
    uint8_t protection;
} __attribute__((packed));

// Measure a masked LAPIC countdown over ~10ms. CPUID.15 supplies the actual
// TSC/crystal ratio; CPUID.16's CPU base frequency is not a guaranteed TSC rate.
// If the ratio is unavailable, use the HPET counter without enabling its IRQs.
static bool calibrate_local_timer(uint64_t& tsc_frequency, uint32_t& count_per_ms) {
    uint32_t a, b, c, d;
    timer_cpuid(0, a, b, c, d);
    uint32_t max_leaf = a;
    tsc_frequency = 0;
    if (max_leaf >= 0x15) {
        timer_cpuid(0x15, a, b, c, d);
        if (a && b && c) tsc_frequency = (uint64_t)c * b / a;
    }

    volatile uint64_t* hpet = nullptr;
    uint64_t hpet_config = 0, hpet_hz = 0, hpet_target = 0;
    if (!tsc_frequency) {
        auto* table = (TimerHPETTable*)find_acpi_table("HPET");
        if (!table || table->header.length < sizeof(TimerHPETTable) ||
            table->address.address_space != 0 || !table->address.address ||
            (table->address.address & 7)) {
            print((char*)"[timer] No CPUID TSC ratio or usable HPET reference.");
            return false;
        }
        uint64_t phys = table->address.address;
        map_mmio_region(phys & ~4095ULL, TIMER_MMIO_BASE + 0x1000, 0x2000);
        hpet = (volatile uint64_t*)(TIMER_MMIO_BASE + 0x1000 + (phys & 4095));
        uint64_t cap = hpet[0];
        uint32_t period = cap >> 32;
        if (!period || period > 100000000 || cap == ~0ULL) return false;
        hpet_hz = 1000000000000000ULL / period;
        hpet_target = hpet_hz / 100;
        hpet_config = hpet[0x10 / 8];
        // Poll only: disable legacy replacement while measuring, then restore.
        hpet[0x10 / 8] = (hpet_config & ~2ULL) | 1;
    }

    timer_apic_write(0x320, (1u << 16) | LOCAL_TIMER_VECTOR); // masked one-shot
    timer_apic_write(0x3E0, 3); // divide by 16
    timer_apic_write(0x380, 0xFFFFFFFF);
    uint32_t apic_start = timer_apic_read(0x390);
    uint32_t hpet_start = hpet ? (uint32_t)hpet[0xF0 / 8] : 0;
    uint64_t start = _usb_rdtsc();
    uint64_t elapsed, reference_ticks = 0;
    bool completed = false;
    // The TSC bound also stops a broken/stalled HPET. No tick IRQ is needed.
    uint64_t limit = tsc_frequency ? tsc_frequency : 3000000000ULL;
    for (uint32_t tries = 0; tries < 10000000; tries++) {
        elapsed = _usb_rdtsc() - start;
        reference_ticks = hpet ? (uint32_t)((uint32_t)hpet[0xF0 / 8] - hpet_start) : elapsed;
        if (reference_ticks >= (hpet ? hpet_target : tsc_frequency / 100)) {
            completed = true;
            break;
        }
        if (elapsed > limit) break;
        __asm__ volatile("pause");
    }
    uint32_t apic_end = timer_apic_read(0x390);
    timer_apic_write(0x380, 0);
    if (hpet) hpet[0x10 / 8] = hpet_config;
    if (!completed || !elapsed || !apic_end || apic_start <= apic_end) return false;
    uint64_t apic_ticks = apic_start - apic_end;
    uint64_t count;
    if (hpet) {
        tsc_frequency = elapsed * hpet_hz / reference_ticks;
        count = apic_ticks * hpet_hz / reference_ticks / 1000;
    } else {
        count = apic_ticks * tsc_frequency / elapsed / 1000;
    }
    if (!count || count > 0xFFFFFFFF || tsc_frequency < 1000000) return false;
    count_per_ms = (uint32_t)count;
    print(hpet ? (char*)"[timer] Reference: HPET." : (char*)"[timer] Reference: CPUID TSC ratio.");
    return true;
}

// Called with interrupts disabled, before the scheduler starts, after PMM/VMM.
static bool init_local_apic_timer(uint64_t& tsc_frequency) {
    uint32_t a, b, c, d;
    timer_cpuid(1, a, b, c, d);
    if (!(d & (1u << 9))) return false;
    uint64_t base = rdmsr(0x1B);
    g_timer_x2apic = (base & (1u << 10)) != 0;
    if (!g_timer_x2apic) {
        uint64_t phys = base & 0xFFFFFF000ULL;
        if (!phys) return false;
        map_mmio_region(phys, TIMER_MMIO_BASE, 4096);
        g_timer_lapic = (volatile uint32_t*)TIMER_MMIO_BASE;
    }
    wrmsr(0x1B, base | (1u << 11)); // global APIC enable, retain mode and BSP bit
    timer_apic_write(0x320, (1u << 16) | LOCAL_TIMER_VECTOR);
    timer_apic_write(0xF0, (timer_apic_read(0xF0) & ~0xFFu) | (1u << 8) | LOCAL_SPURIOUS_VECTOR);
    timer_apic_write(0x80, 0); // TPR: allow timer vector through
    uint32_t count_per_ms = 0;
    if (!calibrate_local_timer(tsc_frequency, count_per_ms)) return false;
    g_local_timer_handler = local_timer_irq_handler;
    timer_apic_write(0x320, (1u << 17) | LOCAL_TIMER_VECTOR); // periodic, unmasked
    timer_apic_write(0x380, count_per_ms);
    print(g_timer_x2apic ? (char*)"[timer] x2APIC timer configured at 1000 Hz." :
                          (char*)"[timer] Local APIC timer configured at 1000 Hz.");
    return true;
}

#endif
