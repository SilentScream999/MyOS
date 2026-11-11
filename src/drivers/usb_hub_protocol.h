#ifndef WYZAR_USB_HUB_PROTOCOL_H
#define WYZAR_USB_HUB_PROTOCOL_H
#include <stdint.h>

// USB 2 chapter 11 / USB 3 chapter 10. A USB 3 hub is a different logical
// device from its USB 2 companion, even when both are in the same enclosure.
static inline uint8_t usb_hub_descriptor_type(uint32_t speed) { return speed >= 4 ? 0x2a : 0x29; }
static inline uint16_t usb_hub_change_mask(uint32_t speed) { return speed >= 4 ? 0xf9 : 0x3f; }
static inline uint16_t usb_hub_change_feature(uint32_t speed, uint8_t bit) {
    if (speed >= 4) {
        if (bit == 0 || bit == 3 || bit == 4) return 16 + bit;
        if (bit == 5) return 29; // C_BH_PORT_RESET
        if (bit == 6) return 25; // C_PORT_LINK_STATE
        if (bit == 7) return 26; // C_PORT_CONFIG_ERROR
        return 0xffff;
    }
    return bit < 5 ? 16 + bit : bit == 5 ? 23 : 0xffff;
}
static inline int usb_hub_route_depth(uint32_t route) {
    if (route & ~0xfffffu) return -1;
    int depth = 0;
    while (route & 15u) { ++depth; route >>= 4; }
    return route ? -1 : depth; // Gaps are not valid route strings.
}
static inline uint8_t usb_hub_child_speed(uint32_t hub_speed, uint16_t status) {
    if ((status & 3) != 3) return 0;
    // xHCI 1.22 4.19.9: SSP hubs return the legacy SS speed encoding here.
    // Address with SS; SSP-capable controllers consume the device's Sublink
    // Speed notification and publish the actual speed in the output context.
    if (hub_speed >= 4 && hub_speed <= 7) return (status & 0x1c00) == 0 ? 4 : 0;
    if (hub_speed > 7) return 0;
    if ((status & 0x600) == 0x600) return 0;
    return status & 0x200 ? 2 : status & 0x400 ? 3 : 1;
}
static inline bool usb_hub_descriptor_valid(uint32_t speed, const uint8_t* d, uint32_t size) {
    uint32_t minimum = speed >= 4 ? 12 : 7;
    if (size < minimum || d[0] < minimum || d[0] > size || d[1] != usb_hub_descriptor_type(speed)) return false;
    return d[2] && d[2] <= (speed >= 4 ? 15 : 16);
}
struct USBSSCompanion { uint8_t burst; uint16_t interval_bytes; };
static inline bool usb_ss_companion(const volatile uint8_t* config, uint32_t length,
                                     uint32_t endpoint, USBSSCompanion& out) {
    out = {};
    if (endpoint > length || length - endpoint < 7 || config[endpoint] < 7 || config[endpoint + 1] != 5) return false;
    uint32_t next = endpoint + config[endpoint];
    if (next > length || length - next < 6 || config[next] != 6 || config[next + 1] != 0x30) return false;
    uint16_t mps = config[endpoint + 4] | ((uint16_t)config[endpoint + 5] << 8);
    uint8_t kind = config[endpoint + 3] & 3;
    uint8_t burst = config[next + 2], attributes = config[next + 3];
    uint16_t bytes = config[next + 4] | ((uint16_t)config[next + 5] << 8);
    if (!mps || mps > 1024 || burst > 15 || attributes) return false;
    if (kind == 2) {
        // BOT uses no streams. USB 3 bulk endpoint packet size is 1024 bytes.
        if (mps != 1024 || bytes) return false;
    } else if (kind == 3) {
        if (!bytes || bytes > (uint32_t)mps * (burst + 1)) return false;
    } else return false;
    out.burst = burst; out.interval_bytes = bytes; return true;
}
#endif
