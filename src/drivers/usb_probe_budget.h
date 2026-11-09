#ifndef WYZAR_USB_PROBE_BUDGET_H
#define WYZAR_USB_PROBE_BUDGET_H
// Included after the controller table and TSC clock helpers. Nested hubs
// share the outer deadline; synchronous BOT readiness keeps its own policy.
struct USBProbeState { uint64_t deadline; bool warned, timed_out; };
static USBProbeState usb_probe_state[MAX_CONTROLLERS] = {};
static uint64_t usb_timeout_deadline(XHCIController* hc, uint64_t milliseconds) {
    uint64_t end = _usb_rdtsc() + (_usb_tsc_hz() / 1000) * milliseconds;
    uint64_t probe = usb_probe_state[hc - xhci_hc].deadline;
    return probe && probe < end ? probe : end;
}
static bool usb_probe_expired(XHCIController* hc) {
    USBProbeState& probe = usb_probe_state[hc - xhci_hc];
    if (!probe.deadline || _usb_rdtsc() < probe.deadline) return false;
    if (!probe.warned) {
        print((char*)"[usb] Enumeration deadline reached; probe stopped, other ports retained.");
        probe.warned = true;
    }
    return true;
}
struct USBProbeGuard {
    USBProbeState& probe;
    bool owner;
    USBProbeGuard(XHCIController* hc) : probe(usb_probe_state[hc - xhci_hc]), owner(!probe.deadline) {
        if (owner) { probe.deadline = usb_timeout_deadline(hc, 3000); probe.warned = probe.timed_out = false; }
    }
    ~USBProbeGuard() { if (owner) { probe.deadline = 0; probe.warned = false; } }
};
// A failed probe still needs a bounded attempt to stop its endpoints. Expired
// probing time must not leave a timed-out control transfer polling forever.
struct USBProbeCleanup {
    USBProbeState& probe;
    uint64_t saved;
    bool warned;
    USBProbeCleanup(XHCIController* hc) : probe(usb_probe_state[hc - xhci_hc]), saved(probe.deadline), warned(probe.warned) {
        probe.deadline = _usb_rdtsc() + _usb_tsc_hz(); probe.warned = false;
    }
    ~USBProbeCleanup() { probe.deadline = saved; probe.warned = warned; }
};
// BOT has its own bounded cold-media readiness policy; do not cut a healthy
// disk's five-second startup allowance down to the hub/HID probing budget.
struct USBProbePause {
    USBProbeState& probe;
    uint64_t saved, started;
    USBProbePause(XHCIController* hc) : probe(usb_probe_state[hc - xhci_hc]), saved(probe.deadline), started(_usb_rdtsc()) { probe.deadline = 0; }
    ~USBProbePause() { probe.deadline = saved ? saved + (_usb_rdtsc() - started) : 0; }
};

static bool usb_wait_register(XHCIController* hc, volatile uint32_t* reg, uint32_t mask,
                              uint32_t expected, uint64_t milliseconds, const char* operation) {
    uint64_t end = usb_timeout_deadline(hc, milliseconds);
    do {
        uint32_t value = *reg;
        if (value != 0xffffffffu && (value & mask) == expected) return true;
        __asm__ volatile("pause");
    } while (_usb_rdtsc() < end);
    print((char*)"[usb] Controller register timeout:"); print((char*)operation);
    hc->needsResetting = true;
    hc->unavailable = true;
    return false;
}

#endif
