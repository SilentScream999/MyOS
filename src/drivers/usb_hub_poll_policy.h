#ifndef WYZAR_USB_HUB_POLL_POLICY_H
#define WYZAR_USB_HUB_POLL_POLICY_H
#include <stdint.h>

struct USBHubPollSchedule {
    uint32_t pending; // Bit 0: hub status; bit N: downstream port N.
    uint64_t audit_after, pending_after;
    uint8_t cursor;
};

static uint32_t usb_hub_poll_plan(USBHubPollSchedule& schedule, uint8_t ports,
                                 uint64_t now, bool& audited) {
    audited = false;
    if (!ports || ports > 16) return 0;
    if (schedule.pending && now < schedule.pending_after) return 0;
    uint32_t mask = schedule.pending & ((1u << (ports + 1)) - 1u);
    if (now >= schedule.audit_after) {
        audited = true;
        if (!schedule.cursor || schedule.cursor > ports) schedule.cursor = 1;
        mask |= 1u << schedule.cursor;
        if (++schedule.cursor > ports) {
            schedule.cursor = 1;
            mask |= 1u; // Hub-level audit once per complete sweep.
        }
    }
    return mask;
}

static void usb_hub_poll_finish(USBHubPollSchedule& schedule, uint64_t finished,
                                 uint64_t ticks_per_ms, bool audited, bool retry) {
    // Schedule from completion, not scan start: slow USB requests must not
    // leave the next deadline already expired and create back-to-back scans.
    if (audited) schedule.audit_after = finished + ticks_per_ms * 250u;
    schedule.pending_after = schedule.pending ? finished + ticks_per_ms * (retry ? 250u : 50u) : 0;
}
#endif
