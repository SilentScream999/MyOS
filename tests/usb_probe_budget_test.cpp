#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include "src/core/structures.h"
#define MAX_CONTROLLERS 4
static XHCIController xhci_hc[MAX_CONTROLLERS] = {};
static uint64_t now = 0;
static bool tick = false;
static volatile uint32_t* transition = nullptr;
static uint64_t transition_at;
static uint64_t _usb_rdtsc() {
    if (tick) ++now;
    if (transition && now >= transition_at) *transition = 2;
    return now;
}
static uint64_t _usb_tsc_hz() { return 1000; }
static std::string messages;
static void print(char* text) { messages += text; messages += '\n'; }
#include "src/drivers/usb_probe_budget.h"
#include "src/drivers/xhci_legacy.h"

int main() {
    {
        USBProbeGuard outer(&xhci_hc[0]);
        assert(usb_probe_state[0].deadline == 3000);
        now = 2000;
        { USBProbeGuard inner(&xhci_hc[0]); assert(usb_probe_state[0].deadline == 3000); }
        assert(usb_probe_state[0].deadline == 3000);
        { USBProbeGuard another_controller(&xhci_hc[1]); assert(usb_probe_state[1].deadline == 5000); }
        assert(!usb_probe_expired(&xhci_hc[0]));
        now = 3000; assert(usb_probe_expired(&xhci_hc[0]));
        size_t first = messages.size(); assert(usb_probe_expired(&xhci_hc[0])); assert(messages.size() == first);
    }
    assert(usb_probe_state[0].deadline == 0);
    {
        USBProbeGuard probe(&xhci_hc[0]);
        uint64_t deadline = usb_probe_state[0].deadline;
        now = deadline;
        assert(usb_probe_expired(&xhci_hc[0]));
        { USBProbeCleanup stop_endpoints(&xhci_hc[0]); assert(usb_timeout_deadline(&xhci_hc[0], 5000) == now + 1000); }
        assert(usb_probe_state[0].deadline == deadline);
    }
    now = 0;
    {
        USBProbeGuard probe(&xhci_hc[0]);
        now = 1000;
        { USBProbePause media(&xhci_hc[0]); assert(usb_probe_state[0].deadline == 0); now = 6000; }
        assert(usb_probe_state[0].deadline == 8000);
        now = 7000; assert(!usb_probe_expired(&xhci_hc[0]));
        assert(usb_timeout_deadline(&xhci_hc[0], 5000) == 8000);
    }
    volatile uint32_t reg = 0;
    now = 0; tick = true;
    transition = &reg; transition_at = 5;
    assert(usb_wait_register(&xhci_hc[0], &reg, 2, 2, 10, "ready"));
    assert(!xhci_hc[0].unavailable && now < 10);
    transition = nullptr; reg = 0;
    assert(!usb_wait_register(&xhci_hc[0], &reg, 2, 2, 10, "stuck reset"));
    assert(xhci_hc[0].needsResetting && xhci_hc[0].unavailable);
    reg = 0xffffffffu;
    assert(!usb_wait_register(&xhci_hc[1], &reg, 1, 1, 10, "missing MMIO"));
    tick = false;
    // No SMI enable bit may be set, even if BIOS left every bit enabled.
    constexpr uint32_t enables = 1u | (1u << 4) | (7u << 13);
    assert(!(xhci_legacy_disable_smi(0xffffffffu) & enables));
    assert((xhci_legacy_disable_smi(0) & (7u << 29)) == (7u << 29));
    // Two protocol capabilities precede the legacy capability; Next is relative.
    uint32_t mmio[64] = {};
    mmio[8] = 2u | (4u << 8);
    mmio[12] = 2u | (4u << 8);
    mmio[16] = 1u; mmio[17] = 0xffffffffu;
    xhci_legacy_handoff(8u << 16, (uint64_t)mmio, sizeof(mmio));
    assert(mmio[16] & (1u << 24));
    assert(mmio[17] == xhci_legacy_disable_smi(0xffffffffu));
    // An out-of-range Next never reads beyond the mapped BAR (ASan checks it).
    mmio[8] = 2u | (255u << 8);
    xhci_legacy_handoff(8u << 16, (uint64_t)mmio, sizeof(mmio));
    puts("USB probe: nested deadlines, media allowance, stuck registers, relative capabilities and SMI disabling passed");
}
