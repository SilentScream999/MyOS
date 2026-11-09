#ifndef WYZAR_USB_HUB_WATCH_H
#define WYZAR_USB_HUB_WATCH_H
#include "xhci_port_state.h"
#include "usb_hub_poll_policy.h"
#include "usb_hub_transfer.h"
struct HubStatusWatch {
    XHCIPortWatch ports[17];
    XHCIPortChangeLatch changes[17];
    volatile uint8_t* report;
    USBHubPollSchedule poll;
    USBHubControl control;
    uint32_t scan_mask, recheck_mask;
    uint64_t sampled_at;
    uint16_t clear_bits;
    uint8_t scan_port, phase, status[4];
    bool audited;
    uint8_t num_ports, report_bytes, failures;
    bool ready, pipe_failed, armed, saw_notification;
};
#endif
