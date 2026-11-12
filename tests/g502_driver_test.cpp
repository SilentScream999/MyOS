#define main hid_parser_test_main
#include "hid_mouse_report_test.cpp"
#undef main
#include <cstdlib>
#include <cstring>
#include <string>
#include "src/core/structures.h"

static std::vector<void*> allocations;
static std::vector<USBSetupPacket> requests;
static std::vector<uint8_t> descriptor;
static std::string log_messages;
static bool fail_descriptor = false, fail_protocol = false;
static uint64_t* alloc_table() {
    void* page = std::calloc(1,4096); assert(page);
    allocations.push_back(page);
    return (uint64_t*)page;
}
static void print(char* message) { log_messages += message; log_messages += '\n'; }
static USB_Response do_control_transfer(USBDevice&, USBSetupPacket* request, volatile uint8_t* buffer, uint16_t length) {
    requests.push_back(*request);
    static TRB event;
    event.status = 1u << 24;
    if (request->bRequest == 6) {
        if (fail_descriptor) event.status = 6u << 24;
        else {
            assert(length == descriptor.size());
            for (size_t b = 0; b < length; ++b) buffer[b] = descriptor[b];
        }
    } else if (fail_protocol) event.status = 6u << 24;
    USB_Response result = {};
    result.gotresponse = true; result.event = &event;
    return result;
}
static bool usb_control_ok(const USB_Response& r) { return r.gotresponse && (r.event->status >> 24) == 1; }
#include "src/drivers/logitech_g502.h"

int main() {
    USBDevice device = {};
    device.vendor_id = 0x046d; device.product_id = 0xc08b;
    assert(usb_is_g502_hero(device));
    device.product_id = 0xc08d; assert(!usb_is_g502_hero(device)); device.product_id = 0xc08b;
    HIDInterface interface = {};
    interface.type = 2; interface.protocol = 2; interface.boot_capable = true;
    interface.iface_num = 7; interface.report_descriptor_length = sizeof(pointer);
    descriptor.assign(pointer, pointer + sizeof(pointer));
    usb_g502_prepare(device, interface);
    assert(interface.type == 2 && interface.mouse_layout && interface.mouse_layout->valid);
    assert(requests.size() == 2);
    assert(requests[0].bmRequestType == 0x81 && requests[0].wValue == 0x2200 && requests[0].wIndex == 7);
    assert(requests[1].bmRequestType == 0x21 && requests[1].bRequest == 0x0b && requests[1].wValue == 1 && requests[1].wIndex == 7);
    assert(log_messages.find("G502 HERO pointer ready") != std::string::npos);
    HIDMouseEvent event;
    uint8_t held_drag[] = {1,0,0x2c,1,0xd4,0xfe,0,0};
    assert(hid_mouse_decode(*interface.mouse_layout, held_drag, 8, interface.held_buttons, event));
    assert(event.buttons == 1 && event.dx == 300 && event.dy == -300);
    // A second HID++/keyboard interface cannot become a spurious mouse.
    const uint8_t auxiliary[] = {5,1,9,6,0xa1,1,0x85,1,5,7,0x19,0xe0,0x29,0xe7,0x15,0,0x25,1,0x75,1,0x95,8,0x81,2,0xc0};
    descriptor.assign(auxiliary, auxiliary + sizeof(auxiliary));
    requests.clear(); interface.report_descriptor_length = sizeof(auxiliary); interface.iface_num = 3;
    usb_g502_prepare(device, interface);
    assert(interface.type == 3 && !interface.mouse_layout && requests.size() == 1);
    assert(requests[0].wIndex == 3);
    // Invalid descriptors or failed protocol selection never use guessed offsets.
    descriptor.assign(pointer, pointer + sizeof(pointer));
    interface.report_descriptor_length = sizeof(pointer);
    fail_protocol = true; usb_g502_prepare(device, interface);
    assert(interface.type == 3 && !interface.mouse_layout);
    fail_protocol = false; fail_descriptor = true; usb_g502_prepare(device, interface);
    assert(interface.type == 3 && !interface.mouse_layout);
    // Wireless keyboard/trackpad combos commonly prefix an 8-bit relative mouse
    // report with ID 1. Treating it as plain boot input makes X appear as Y.
    const uint8_t combo[] = {
        5,1,9,2,0xa1,1,0x85,1,5,9,0x19,1,0x29,3,0x15,0,0x25,1,0x75,1,0x95,3,0x81,2,
        0x75,5,0x95,1,0x81,3,5,1,9,0x30,9,0x31,0x15,0x81,0x25,0x7f,0x75,8,0x95,2,0x81,6,
        9,0x38,0x95,1,0x81,6,0xc0
    };
    fail_descriptor = false;
    device.vendor_id = 0x1234; device.product_id = 0x5678;
    descriptor.assign(combo, combo + sizeof(combo));
    interface.type = 2; interface.mouse_layout = nullptr;
    interface.report_descriptor_length = sizeof(combo); interface.iface_num = 2;
    requests.clear();
    assert(usb_hid_pointer_prepare(device, interface));
    assert(interface.type == 2 && interface.mouse_layout && interface.mouse_layout->numbered);
    assert(requests.size() == 2 && requests[1].wValue == 1 && requests[1].wIndex == 2);
    uint8_t trackpad[] = {1,0,12,0xf7,0};
    assert(hid_mouse_decode(*interface.mouse_layout, trackpad, sizeof(trackpad), interface.held_buttons, event));
    assert(event.dx == 12 && event.dy == -9 && event.buttons == 0);
    trackpad[1] = 1;
    assert(hid_mouse_decode(*interface.mouse_layout, trackpad, sizeof(trackpad), interface.held_buttons, event));
    assert(event.dx == 12 && event.dy == -9 && event.buttons == 1);
    puts("Composite wireless pointer: prefixed 8-bit X/Y and held buttons passed");
    for (void* page : allocations) std::free(page);
    puts("G502 HERO setup: correct interface/protocol, pointer decode, auxiliary rejection and failure handling passed");
}
