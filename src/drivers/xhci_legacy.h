#ifndef WYZAR_XHCI_LEGACY_H
#define WYZAR_XHCI_LEGACY_H
#include <stdint.h>

static uint32_t xhci_legacy_disable_smi(uint32_t value) {
    // USBLEGCTLSTS mixes enable bits and W1C status. Writing FFFFFFFF enables
    // firmware SMIs again. Preserve the defined reserved/RO fields, disable all
    // SMI enables, and acknowledge the ownership/PCI/BAR status bits (29..31).
    constexpr uint32_t preserve = (7u << 1) | (255u << 5) | (7u << 17);
    return (value & preserve) | (7u << 29);
}

static void xhci_legacy_handoff(uint32_t hcc1, uint64_t mmio_base, uint64_t mmio_size) {
    uint32_t offset = (hcc1 >> 16) & 0xffffu;
    for (unsigned seen = 0; offset && seen < 256; ++seen) {
        if ((uint64_t)offset * 4u + 8u > mmio_size) {
            print((char*)"[usb] Invalid xHCI extended capability offset.");
            return;
        }
        volatile uint32_t* capability = (volatile uint32_t*)(mmio_base + (uint64_t)offset * 4u);
        uint32_t header = capability[0];
        uint32_t next = (header >> 8) & 255u;
        if ((header & 255u) == 1) {
            capability[0] = header | (1u << 24);
            for (int attempt = 0; attempt < 1000000 && (capability[0] & (1u << 16)); ++attempt)
                __asm__ volatile("pause");
            capability[1] = xhci_legacy_disable_smi(capability[1]);
            return;
        }
        // xECP is absolute; each capability's Next field is relative in DWORDs.
        offset = next ? offset + next : 0;
    }
}
#endif
