#ifndef WYZAR_USB_STORAGE_COMPLETION_H
#define WYZAR_USB_STORAGE_COMPLETION_H
#include <stdint.h>
struct USBStorageCompletion {
    uint64_t pointer;
    uint32_t status;
    uint8_t type, slot, dci;
    bool active, complete;
};
static inline bool usb_storage_completion_arm(USBStorageCompletion& c, uint8_t type,
                                              uint8_t slot, uint8_t dci, uint64_t pointer) {
    if (c.active) return false;
    c = {pointer, 0, type, slot, dci, true, false};
    return true;
}
static inline bool usb_storage_completion_event(USBStorageCompletion& c, uint8_t type,
                                                uint64_t pointer, uint32_t ctrl, uint32_t status) {
    if (!c.active || c.complete || type != c.type || pointer != c.pointer ||
        ((ctrl >> 24) & 255) != c.slot || (type == 32 && ((ctrl >> 16) & 31) != c.dci)) return false;
    c.status = status; c.complete = true; return true;
}
#endif
