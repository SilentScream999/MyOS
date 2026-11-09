#ifndef WYZAR_HID_POINTER_SETUP_H
#define WYZAR_HID_POINTER_SETUP_H
#include "hid_mouse_report.h"

// Returns true when the descriptor decides how to handle this interface.
// False allows the existing boot fallback for unavailable/unsupported layouts.
static bool usb_hid_pointer_prepare(USBDevice& dev, HIDInterface& interface) {
    interface.mouse_layout = nullptr;
    interface.held_buttons = 0;
    uint16_t length = interface.report_descriptor_length;
    if (!length || length > 4096) return false;
    volatile uint8_t* descriptor = (volatile uint8_t*)alloc_table();
    for (uint16_t b = 0; b < length; ++b) descriptor[b] = 0;
    USBSetupPacket request = {};
    request.bmRequestType = 0x81; request.bRequest = 6;
    request.wValue = 0x2200; request.wIndex = interface.iface_num; request.wLength = length;
    if (!usb_control_ok(do_control_transfer(dev, &request, descriptor, length))) {
        print((char*)"[hid] Pointer report descriptor read failed; interface inactive.");
        interface.type = 3;
        return true;
    }
    HIDMouseLayout* layout = (HIDMouseLayout*)alloc_table();
    if (!hid_mouse_parse(descriptor, length, *layout)) {
        if (layout->parsed && !layout->pointer_collection) {
            interface.type = 3;
            print((char*)"[hid] Non-pointer report interface ignored.");
            return true;
        }
        print((char*)"[hid] Pointer descriptor unsupported; using existing fallback.");
        return false;
    }
    if (interface.boot_capable) {
        request.bmRequestType = 0x21; request.bRequest = 0x0b;
        request.wValue = 1; request.wLength = 0;
        if (!usb_control_ok(do_control_transfer(dev, &request, nullptr, 0))) {
            interface.type = 3;
            print((char*)"[hid] Pointer report protocol selection failed; interface inactive.");
            return true;
        }
    }
    interface.mouse_layout = layout;
    interface.held_buttons = 0;
    interface.type = 2;
    print((char*)"[hid] Relative pointer ready (descriptor-based reports).");
    return true;
}
#endif
