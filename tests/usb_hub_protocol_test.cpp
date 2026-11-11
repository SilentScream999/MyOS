#include <cassert>
#include <cstdio>
#include "src/drivers/usb_hub_protocol.h"
int main() {
    assert(usb_hub_descriptor_type(3) == 0x29 && usb_hub_descriptor_type(4) == 0x2a);
    assert(usb_hub_child_speed(4, 0x203) == 4); // SS power must not become LS speed.
    assert(usb_hub_child_speed(5, 0x203) == 4);
    assert(usb_hub_child_speed(6, 0x203) == 4 && usb_hub_child_speed(7, 0x203) == 4);
    assert(usb_hub_child_speed(8, 0x203) == 0);
    assert(usb_hub_child_speed(4, 0x201) == 0 && usb_hub_child_speed(4, 0x603) == 0);
    assert(usb_hub_child_speed(3, 0x203) == 2 && usb_hub_child_speed(3, 0x403) == 3);
    assert(usb_hub_child_speed(3, 3) == 1 && usb_hub_child_speed(3, 0x603) == 0);
    assert(usb_hub_route_depth(0) == 0 && usb_hub_route_depth(0x21) == 2);
    assert(usb_hub_route_depth(0x101) == -1 && usb_hub_route_depth(0xfffff) == 5);
    assert(usb_hub_change_mask(4) == 0xf9 && usb_hub_change_mask(3) == 0x3f);
    assert(usb_hub_change_feature(4, 5) == 29 && usb_hub_change_feature(4, 6) == 25);
    assert(usb_hub_change_feature(4, 7) == 26 && usb_hub_change_feature(3, 5) == 23);
    uint8_t bulk[] = {7,5,0x81,2,0,4,0, 6,0x30,15,0,0,0};
    USBSSCompanion c{};
    assert(usb_ss_companion(bulk, sizeof(bulk), 0, c) && c.burst == 15);
    assert(!usb_ss_companion(bulk, sizeof(bulk)-1, 0, c));
    assert(!usb_ss_companion(bulk, sizeof(bulk), UINT32_MAX, c));
    bulk[9] = 16; assert(!usb_ss_companion(bulk, sizeof(bulk), 0, c)); bulk[9] = 0;
    bulk[10] = 1; assert(!usb_ss_companion(bulk, sizeof(bulk), 0, c)); bulk[10] = 0;
    uint8_t interrupt[] = {7,5,0x81,3,2,0,12, 6,0x30,0,0,2,0};
    assert(usb_ss_companion(interrupt, sizeof(interrupt), 0, c) && c.interval_bytes == 2);
    interrupt[11] = 3; assert(!usb_ss_companion(interrupt, sizeof(interrupt), 0, c));
    uint8_t descriptor[12] = {12,0x2a,4};
    assert(usb_hub_descriptor_valid(4, descriptor, sizeof(descriptor)));
    descriptor[2] = 16; assert(!usb_hub_descriptor_valid(4, descriptor, sizeof(descriptor)));
    std::puts("USB hub protocol: descriptor bounds, SS companions, speed encoding, route depth and change selectors passed");
}
