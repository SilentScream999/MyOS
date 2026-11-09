#ifndef WYZAR_USB_HUB_TRANSFER_H
#define WYZAR_USB_HUB_TRANSFER_H
#include <stdint.h>

// Persistent DMA storage and copied completion fields: no event-ring pointers
// survive a dispatcher pass. One background EP0 request per registered hub.
struct USBHubControl {
    volatile uint8_t* data;
    uint64_t setup_trb, data_trb, status_trb, deadline;
    uint32_t code;
    uint16_t length, actual;
    bool active, complete;
};

static bool usb_hub_control_event(USBHubControl& request, uint8_t slot,
                                  uint32_t event_control, uint64_t pointer,
                                  uint32_t event_status) {
    if (!request.active || ((event_control >> 24) & 255u) != slot ||
        ((event_control >> 16) & 31u) != 1u) return false;
    if (pointer != request.setup_trb && pointer != request.status_trb &&
        (!request.length || pointer != request.data_trb)) return false;
    if (request.complete) return true; // Duplicate completion; retain first result.
    uint32_t code = event_status >> 24;
    if (pointer == request.data_trb && request.length && code == 13) {
        uint32_t residual = event_status & 0xffffffu;
        request.actual = residual <= request.length ? request.length - residual : 0;
    }
    if ((code != 1 && code != 13) || pointer == request.status_trb) {
        request.code = code;
        request.complete = true;
    }
    return true;
}
#endif
