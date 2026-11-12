#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "src/drivers/hid_mouse_report.h"

// Logitech G502-family relative pointer layout: sixteen buttons, signed
// sixteen-bit X/Y, eight-bit wheel and consumer horizontal pan (8 bytes).
static const uint8_t pointer[] = {
    0x05,0x01,0x09,0x02,0xa1,0x01,0x09,0x01,0xa1,0x00,
    0x95,0x10,0x75,0x01,0x15,0x00,0x25,0x01,0x05,0x09,0x19,0x01,0x29,0x10,0x81,0x02,
    0x95,0x02,0x75,0x10,0x16,0x01,0x80,0x26,0xff,0x7f,0x05,0x01,0x09,0x30,0x09,0x31,0x81,0x06,
    0x95,0x01,0x75,0x08,0x15,0x81,0x25,0x7f,0x09,0x38,0x81,0x06,
    0x95,0x01,0x05,0x0c,0x0a,0x38,0x02,0x81,0x06,0xc0,0xc0
};
static void set_delta(uint8_t* packet, int16_t dx, int16_t dy) {
    packet[2] = dx; packet[3] = (uint16_t)dx >> 8;
    packet[4] = dy; packet[5] = (uint16_t)dy >> 8;
}
int main() {
    HIDMouseLayout layout;
    assert(hid_mouse_parse(pointer, sizeof(pointer), layout));
    assert(!layout.numbered && layout.count == 1);
    assert(layout.reports[0].input_bytes == 8);
    assert(layout.reports[0].x.offset == 16 && layout.reports[0].y.offset == 32);
    uint8_t held = 0;
    HIDMouseEvent event;
    uint8_t packet[8] = {};
    for (int buttons = 0; buttons < 256; ++buttons) {
        packet[0] = buttons; set_delta(packet, 321, -701); packet[6] = 0xff;
        assert(hid_mouse_decode(layout, packet, sizeof(packet), held, event));
        assert(event.buttons == buttons && event.dx == 321 && event.dy == -701 && event.wheel == -1);
    }
    // A held drag must not release/re-grab as packet byte 0 changes interpretation.
    packet[0] = 1;
    int presses = 0, releases = 0;
    held = 0;
    for (int frame = 0; frame < 1000; ++frame) {
        set_delta(packet, frame % 23 - 11, 15 - frame % 31);
        uint8_t previous = held;
        assert(hid_mouse_decode(layout, packet, 8, held, event));
        presses += !(previous & 1) && (held & 1);
        releases += (previous & 1) && !(held & 1);
        assert(event.buttons & 1);
    }
    assert(presses == 1 && releases == 0);
    for (size_t length = 0; length < 8; ++length) {
        packet[0] = 0;
        assert(!hid_mouse_decode(layout, packet, length, held, event));
        assert(held == 1);
    }
    assert(hid_mouse_decode(layout, packet, 8, held, event) && held == 0);
    set_delta(packet, -32767, 32767);
    assert(hid_mouse_decode(layout, packet, 8, held, event));
    assert(event.dx == -32767 && event.dy == 32767);
    // Numbered reports: vendor/keyboard IDs must not alter held pointer buttons.
    std::vector<uint8_t> numbered = {0x85, 7};
    numbered.insert(numbered.end(), pointer, pointer + sizeof(pointer));
    const uint8_t vendor[] = {0x06,0,0xff,0x09,2,0xa1,1,0x85,0x11,0x75,8,0x95,0x13,0x09,2,0x81,0,0xc0};
    numbered.insert(numbered.end(), vendor, vendor + sizeof(vendor));
    assert(hid_mouse_parse(numbered.data(), numbered.size(), layout));
    uint8_t with_id[9] = {7,1,0,0x80,0,0x80,0xff,0,0};
    assert(hid_mouse_decode(layout, with_id, 9, held, event));
    assert(event.dx == 128 && event.dy == -128 && held == 1);
    uint8_t unknown[20] = {0x11};
    assert(!hid_mouse_decode(layout, unknown, 20, held, event) && held == 1);
    unknown[0] = 1;
    assert(!hid_mouse_decode(layout, unknown, 20, held, event) && held == 1);
    HIDMouseLayout not_mouse;
    assert(!hid_mouse_parse(vendor, sizeof(vendor), not_mouse));
    // Split reports preserve buttons when only motion arrives.
    const uint8_t split[] = {
        5,1,9,2,0xa1,1,0x85,1,9,0x30,9,0x31,0x15,0x81,0x25,0x7f,0x75,8,0x95,2,0x81,6,
        0x85,2,5,9,0x19,1,0x29,8,0x15,0,0x25,1,0x75,1,0x95,8,0x81,2,0xc0
    };
    assert(hid_mouse_parse(split, sizeof(split), layout));
    uint8_t button_only[] = {2,1}, motion_only[] = {1,5,0xf9};
    assert(hid_mouse_decode(layout, button_only, 2, held, event) && held == 1);
    assert(hid_mouse_decode(layout, motion_only, 3, held, event));
    assert(held == 1 && event.dx == 5 && event.dy == -7);
    // Some combo receivers pack two signed 12-bit axes across byte boundaries.
    const uint8_t packed[] = {
        5,1,9,2,0xa1,1,0x85,2,5,9,0x19,1,0x29,3,0x15,0,0x25,1,0x75,1,0x95,3,0x81,2,
        0x75,5,0x95,1,0x81,3,5,1,9,0x30,9,0x31,0x16,1,0xf8,0x26,0xff,7,0x75,12,0x95,2,0x81,6,0xc0
    };
    assert(hid_mouse_parse(packed, sizeof(packed), layout));
    uint8_t packed_motion[] = {2,1,0,0x3f,0x12};
    assert(hid_mouse_decode(layout, packed_motion, sizeof(packed_motion), held, event));
    assert(event.dx == -256 && event.dy == 291 && event.buttons == 1);
    // Malformed/truncated descriptors and oversized fields fail safely.
    for (size_t n = 0; n < sizeof(pointer); ++n) assert(!hid_mouse_parse(pointer, n, layout));
    uint8_t pop_underflow[] = {0xb4};
    assert(!hid_mouse_parse(pop_underflow, 1, layout));
    uint8_t oversized[] = {0x75,32,0x97,0xff,0xff,0xff,0x7f,0x81,2};
    assert(!hid_mouse_parse(oversized, sizeof(oversized), layout));
    uint8_t id_zero[] = {0x85,0};
    assert(!hid_mouse_parse(id_zero, 2, layout));
    puts("HID mouse: 16-bit axes, all button masks, stable drag, report IDs, truncation and bounds passed");
    return 0;
}
