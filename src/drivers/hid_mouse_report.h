#ifndef WYZAR_HID_MOUSE_REPORT_H
#define WYZAR_HID_MOUSE_REPORT_H
#include <stddef.h>
#include <stdint.h>

// Bounded HID 1.11 input-report parser for relative mouse collections. Offsets
// exclude the optional Report ID byte. Vendor/keyboard reports never release
// pointer buttons. Device configuration (HID++, DPI, RGB) is a separate protocol.
struct HIDMouseField { uint16_t offset; uint8_t bits; bool negative; };
struct HIDMouseReport {
    uint8_t id;
    uint16_t input_bytes;
    HIDMouseField buttons[8], x, y, wheel;
};
struct HIDMouseLayout {
    HIDMouseReport reports[8];
    uint8_t count;
    bool numbered, valid, parsed, pointer_collection;
};
struct HIDMouseEvent { int16_t dx, dy; int8_t wheel; uint8_t buttons; };

static uint32_t hid_mouse_usage(uint32_t value, uint8_t bytes, uint32_t page) {
    return bytes == 4 ? value : (page << 16) | (value & 0xffffu);
}
static int32_t hid_mouse_signed(uint32_t value, uint8_t bytes) {
    if (bytes == 1) return (int8_t)value;
    if (bytes == 2) return (int16_t)value;
    return (int32_t)value;
}
static HIDMouseReport* hid_mouse_report(HIDMouseLayout& layout, uint8_t id) {
    for (uint8_t i = 0; i < layout.count; ++i) if (layout.reports[i].id == id) return &layout.reports[i];
    if (layout.count == 8) return nullptr;
    HIDMouseReport* report = &layout.reports[layout.count++];
    report->id = id;
    return report;
}

static bool hid_mouse_parse(const volatile uint8_t* descriptor, size_t length, HIDMouseLayout& layout) {
    for (size_t i = 0; i < sizeof(layout); ++i) ((volatile uint8_t*)&layout)[i] = 0;
    if (!descriptor || !length || length > 4096) return false;
    struct Globals { uint32_t page, size, count; int32_t minimum; uint8_t id; };
    Globals g = {}, saved[8];
    uint8_t saved_count = 0;
    uint16_t input_bits[256];
    for (int i = 0; i < 256; ++i) input_bits[i] = 0;
    uint32_t usages[32], usage_min = 0, usage_max = 0;
    uint8_t usage_count = 0;
    bool have_min = false, have_max = false, in_mouse = false;
    bool collections[16];
    uint8_t depth = 0;
    size_t cursor = 0;
    while (cursor < length) {
        uint8_t prefix = descriptor[cursor++];
        if (prefix == 0xfe) return false; // unsupported long items: fail closed
        uint8_t bytes = prefix & 3u;
        if (bytes == 3) bytes = 4;
        if (bytes > length - cursor) return false;
        uint32_t value = 0;
        for (uint8_t b = 0; b < bytes; ++b) value |= (uint32_t)descriptor[cursor++] << (b * 8);
        uint8_t type = (prefix >> 2) & 3u, tag = prefix >> 4;
        if (type == 1) {
            if (tag == 0) { if (value > 0xffffu) return false; g.page = value; }
            else if (tag == 1) g.minimum = hid_mouse_signed(value, bytes);
            else if (tag == 7) g.size = value;
            else if (tag == 8) {
                if (!value || value > 255) return false;
                g.id = value; layout.numbered = true;
            } else if (tag == 9) g.count = value;
            else if (tag == 10) {
                if (saved_count == 8) return false;
                Globals& target = saved[saved_count++];
                target.page = g.page; target.size = g.size; target.count = g.count;
                target.minimum = g.minimum; target.id = g.id;
            } else if (tag == 11) {
                if (!saved_count) return false;
                Globals& target = saved[--saved_count];
                g.page = target.page; g.size = target.size; g.count = target.count;
                g.minimum = target.minimum; g.id = target.id;
            }
            continue;
        }
        if (type == 2) {
            uint32_t usage = hid_mouse_usage(value, bytes, g.page);
            if (tag == 0) {
                if (usage_count == 32) return false;
                usages[usage_count++] = usage;
            } else if (tag == 1) { usage_min = usage; have_min = true; }
            else if (tag == 2) { usage_max = usage; have_max = true; }
            else if (tag == 10) return false; // alternative usage delimiters
            continue;
        }
        if (type != 0) return false;
        if (tag == 10) {
            if (depth == 16) return false;
            collections[depth++] = in_mouse;
            uint32_t usage = usage_count ? usages[0] : (have_min ? usage_min : 0);
            if (value == 1 && usage == 0x00010002u) in_mouse = true;
            if (value == 1 && (usage == 0x00010002u || usage == 0x000d0004u || usage == 0x000d0005u))
                layout.pointer_collection = true;
        } else if (tag == 12) {
            if (!depth) return false;
            in_mouse = collections[--depth];
        } else if (tag == 8) {
            uint64_t end = (uint64_t)input_bits[g.id] + (uint64_t)g.size * g.count;
            if (!g.size || !g.count || end > 8192) return false;
            if (in_mouse && (value & 3u) == 2u && g.size <= 32) {
                for (uint32_t i = 0; i < g.count; ++i) {
                    uint32_t usage = 0;
                    if (usage_count) usage = usages[i < usage_count ? i : usage_count - 1];
                    else if (have_min && have_max && usage_max >= usage_min && i <= usage_max - usage_min)
                        usage = usage_min + i;
                    uint16_t page = usage >> 16, item = usage & 0xffffu;
                    HIDMouseField* field = nullptr;
                    if ((page == 9 && item >= 1 && item <= 8) ||
                        (page == 1 && (value & 4u) && (item == 0x30 || item == 0x31 || item == 0x38))) {
                        HIDMouseReport* report = hid_mouse_report(layout, g.id);
                        if (!report) return false;
                        if (page == 9) field = &report->buttons[item - 1];
                        else if (item == 0x30) field = &report->x;
                        else if (item == 0x31) field = &report->y;
                        else field = &report->wheel;
                    }
                    if (field && !field->bits) {
                        field->offset = input_bits[g.id] + i * g.size;
                        field->bits = g.size;
                        field->negative = g.minimum < 0;
                    }
                }
            }
            input_bits[g.id] = end;
        } else if (tag != 9 && tag != 11) return false;
        usage_count = 0; have_min = have_max = false;
    }
    if (depth || saved_count || (layout.numbered && input_bits[0])) return false;
    bool pointer = false;
    for (uint8_t i = 0; i < layout.count; ++i) {
        HIDMouseReport& report = layout.reports[i];
        report.input_bytes = (input_bits[report.id] + 7u) / 8u;
        if (report.x.bits && report.y.bits) pointer = true;
    }
    layout.valid = pointer;
    layout.parsed = true;
    return pointer;
}

static int32_t hid_mouse_field(const HIDMouseField& field, const uint8_t* data) {
    uint32_t value = 0;
    for (uint8_t bit = 0; bit < field.bits; ++bit) {
        uint32_t offset = field.offset + bit;
        value |= (uint32_t)((data[offset / 8] >> (offset % 8)) & 1u) << bit;
    }
    if (field.negative && field.bits && (value & (1u << (field.bits - 1))))
        return (int32_t)((int64_t)value - (1ll << field.bits));
    return value > 0x7fffffffu ? 0x7fffffff : (int32_t)value;
}
static int16_t hid_mouse_delta(int32_t value) {
    return value > 32767 ? 32767 : value < -32768 ? -32768 : (int16_t)value;
}
static bool hid_mouse_decode(const HIDMouseLayout& layout, const uint8_t* packet, size_t length,
                             uint8_t& held_buttons, HIDMouseEvent& event) {
    if (!layout.valid || !packet || !length) return false;
    uint8_t id = layout.numbered ? *packet++ : 0;
    if (layout.numbered) --length;
    const HIDMouseReport* report = nullptr;
    for (uint8_t i = 0; i < layout.count; ++i) if (layout.reports[i].id == id) report = &layout.reports[i];
    if (!report || length < report->input_bytes) return false;
    uint8_t buttons = held_buttons;
    for (uint8_t b = 0; b < 8; ++b) if (report->buttons[b].bits) {
        if (hid_mouse_field(report->buttons[b], packet)) buttons |= 1u << b;
        else buttons &= ~(1u << b);
    }
    event.dx = hid_mouse_delta(hid_mouse_field(report->x, packet));
    event.dy = hid_mouse_delta(hid_mouse_field(report->y, packet));
    int32_t wheel = hid_mouse_field(report->wheel, packet);
    event.wheel = wheel > 127 ? 127 : wheel < -127 ? -127 : (int8_t)wheel;
    event.buttons = held_buttons = buttons;
    return true;
}
#endif
