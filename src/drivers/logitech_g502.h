#ifndef WYZAR_LOGITECH_G502_H
#define WYZAR_LOGITECH_G502_H
#include "hid_pointer_setup.h"

static bool usb_is_g502_hero(const USBDevice& dev) {
    // libratbag's logitech-g502-hero.device identifies the wired HERO as C08B.
    return dev.vendor_id == 0x046d && dev.product_id == 0xc08b;
}

static void usb_g502_prepare(USBDevice& dev, HIDInterface& interface) {
    // The HERO is composite: its keyboard/consumer/HID++ interface is not a
    // second pointer. Never pass that traffic to mouse_process_input().
    interface.type = 3;
    interface.mouse_layout = nullptr;
    if (usb_hid_pointer_prepare(dev, interface) && interface.mouse_layout)
        print((char*)"[hid] Logitech G502 HERO pointer ready (descriptor-based reports).");
}
#endif
