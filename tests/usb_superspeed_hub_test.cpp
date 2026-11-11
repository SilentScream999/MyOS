// Runs the actual hub enumerator with scripted control/command completions.
// Generate the included source slice using run_usb_hub_tests.py.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "src/core/structures.h"
#include "src/drivers/usb_hub_protocol.h"
#include "src/drivers/usb_hub_watch.h"
static constexpr uint64_t HHDM = 0;
static XHCIController controller;
static XHCIController* xhci_hc = &controller;
static USBDevice* usb_slot_devices[1][256];
static USBDevice* hubs[4];
static HubStatusWatch hub_watch[4];
static int hub_count;
static std::vector<void*> allocations;
static std::vector<USBDevice*> disks;
static std::vector<USBDevice*> keyboards;
static std::vector<unsigned> depths, descriptor_types;
static std::string messages;
static uint64_t now;
static uint8_t next_slot;
static uint8_t negotiated_child_speed;
static uint8_t slow_child_speed;
static uint32_t completion_control;
static TRB completion;
static bool nested, reject_depth, malformed_descriptor, reset_pending, no_power_switch;
static unsigned watch_ready;
struct Address { uint32_t slot[4], ep0[5]; };
static std::vector<Address> addressed;
static void print(char* s) { messages += s; }
static void to_str(uint64_t n, char* s) { std::sprintf(s, "%llu", (unsigned long long)n); }
static void to_hex(uint64_t n, char* s) { to_str(n, s); }
static uint64_t _usb_rdtsc() { return ++now; }
static uint64_t _usb_tsc_hz() { return 1000; }
static uint64_t usb_timeout_deadline(XHCIController*, uint64_t ms) { return now + ms; }
static bool usb_probe_expired(XHCIController*) { return false; }
struct USBProbeGuard { USBProbeGuard(XHCIController*) {} };
static void tsc_delay_ms(uint64_t ms) { now += ms; }
static volatile uint64_t* alloc_table() {
    void* p = std::calloc(1, 4096); assert(p); allocations.push_back(p); return (volatile uint64_t*)p;
}
static void ring_init(Ring* r) { r->phys = (uintptr_t)r; }
static void ring_push_cmd(Ring*, uint32_t control, uint64_t parameter, uint32_t) {
    unsigned type = (control >> 10) & 63, slot = control >> 24;
    if (type == 9) slot = next_slot++;
    if (type == 11 || type == 12) {
        auto* in = (uint32_t*)parameter;
        auto* out = (uint32_t*)controller.dcbaa[slot]; assert(out);
        for (unsigned i = 0; i < 8; ++i) out[i] = in[8 + i];
        if (type == 11 && negotiated_child_speed)
            out[0] = (out[0] & ~(15u << 20)) | ((uint32_t)negotiated_child_speed << 20);
        if (type == 11) {
            Address a{};
            for (unsigned i = 0; i < 4; ++i) a.slot[i] = in[8 + i];
            for (unsigned i = 0; i < 5; ++i) a.ep0[i] = in[16 + i];
            addressed.push_back(a);
        }
    }
    completion.status = 1u << 24; completion_control = slot << 24;
}
static USB_Response get_usb_command_response(XHCIController*, unsigned) {
    return {true, &completion, 33, completion_control};
}
static USB_Response usb_wait_command(XHCIController* hc, const char*) { return get_usb_command_response(hc, 1); }
static bool usb_control_ok(const USB_Response& r) { return r.gotresponse && (r.event->status >> 24) == 1; }
static USB_Response do_control_transfer(USBDevice& hub, USBSetupPacket* request, volatile uint8_t* data, unsigned) {
    static TRB response;
    response.status = 1u << 24;
    if (request->bRequest == 6) {
        unsigned type = request->wValue >> 8;
        descriptor_types.push_back(type);
        assert(type == usb_hub_descriptor_type(hub.speed));
        assert(request->wLength == (hub.speed >= 4 ? 12 : 16));
        data[0] = hub.speed >= 4 ? 12 : 9;
        data[1] = malformed_descriptor ? 0 : type;
        data[2] = 1; data[5] = 60; // 120 ms power-good delay.
        data[3] = (no_power_switch ? 2 : 0) | (hub.speed == 3 ? 2u << 5 : 0);
    } else if (request->bRequest == 12) {
        assert(request->bmRequestType == 0x20 && request->wIndex == 0 && request->wLength == 0);
        depths.push_back(request->wValue);
        if (reject_depth) response.status = 6u << 24;
    } else if (request->bRequest == 0) {
        uint16_t status = 3 | (hub.speed >= 4 ? 0x200 : 0x400);
        if (hub.speed == 3 && slow_child_speed)
            status = 3 | 0x100 | (slow_child_speed == 2 ? 0x200 : 0);
        data[0] = status; data[1] = status >> 8;
        data[2] = reset_pending ? 16 : 0; data[3] = 0;
    } else if (request->bRequest == 3 && request->wValue == 8) assert(!no_power_switch);
    else if (request->bRequest == 3 && request->wValue == 4) reset_pending = true;
    else if (request->bRequest == 1 && request->wValue == 20) reset_pending = false;
    return {true, &response, 32, 0};
}
static bool hub_set_port_feature(USBDevice& hub, uint16_t port, uint16_t feature) {
    USBSetupPacket p{0x23, 3, feature, port, 0}; return usb_control_ok(do_control_transfer(hub, &p, nullptr, 0));
}
static bool hub_clear_port_feature(USBDevice& hub, uint16_t port, uint16_t feature) {
    USBSetupPacket p{0x23, 1, feature, port, 0}; return usb_control_ok(do_control_transfer(hub, &p, nullptr, 0));
}
static bool hub_get_port_status(USBDevice& hub, uint16_t port, uint8_t* out) {
    USBSetupPacket p{0xa3, 0, 0, port, 4}; return usb_control_ok(do_control_transfer(hub, &p, out, 4));
}
static void hub_prepare_watch(USBDevice*, uint8_t) { ++watch_ready; }
static bool usb_storage_is_device(USBDevice* dev) {
    for (auto* disk : disks) if (disk == dev) return true; return false;
}
static void enumerate_device_after_address(USBDevice* dev, volatile uint8_t*, uint32_t, uint32_t) {
    if (nested && !dev->parent_hub->parent_hub) { dev->is_hub = true; hubs[hub_count++] = dev; }
    else if (slow_child_speed) { dev->interface_count = 1; keyboards.push_back(dev); }
    else disks.push_back(dev);
}
static void enumerate_hub_children(USBDevice*, uint8_t, uint8_t = 0);
#include "../.storage-test/usb_hub_enumeration_under_test.h"
static USBDevice root;
static void setup(unsigned speed = 4) {
    for (void* p : allocations) std::free(p); allocations.clear();
    controller = {}; root = {}; now = 0; next_slot = 2;
    nested = reject_depth = malformed_descriptor = reset_pending = no_power_switch = false;
    negotiated_child_speed = 0;
    slow_child_speed = 0; keyboards.clear();
    addressed.clear(); disks.clear(); depths.clear(); descriptor_types.clear(); messages.clear(); watch_ready = 0;
    static uint64_t dcbaa[256]; static uint32_t doorbells[256]; static bool failed[4096];
    std::memset(dcbaa, 0, sizeof(dcbaa)); std::memset(failed, 0, sizeof(failed));
    controller.dcbaa = dcbaa; controller.doorbell32 = doorbells; controller.portfailed = failed;
    root.hc = &controller; root.slot_id = 1; root.speed = speed; root.root_port_num = 7;
    root.device_ctx = alloc_table(); root.is_hub = true;
    auto* slot = (volatile uint32_t*)root.device_ctx;
    slot[0] = (speed << 20) | (1u << 27); slot[1] = 7u << 16;
    controller.dcbaa[1] = (uintptr_t)root.device_ctx;
    hubs[0] = &root; hub_count = 1;
}
int main() {
    setup(); enumerate_hub_children(&root, 8, 0);
    assert(disks.size() == 1 && disks[0]->speed == 4);
    assert(depths == std::vector<unsigned>{0} && descriptor_types == std::vector<unsigned>{0x2a});
    assert(addressed.size() == 1 && (addressed[0].slot[0] & 0xfffff) == 1);
    assert(addressed[0].slot[1] == 7u << 16 && addressed[0].slot[2] == 0);
    assert(addressed[0].ep0[1] >> 16 == 512 && watch_ready == 1 && now >= 120);
    assert(((volatile uint32_t*)root.device_ctx)[0] & (1u << 26));
    setup(); no_power_switch = true; enumerate_hub_children(&root, 8, 0);
    assert(disks.size() == 1 && now >= 120);
    setup(); nested = true; enumerate_hub_children(&root, 8, 0);
    assert(disks.size() == 1 && disks[0]->speed == 4);
    assert((depths == std::vector<unsigned>{0, 1}) && watch_ready == 2);
    assert((addressed.back().slot[0] & 0xfffff) == 0x11 && addressed.back().slot[2] == 0);
    setup(3); enumerate_hub_children(&root, 8, 0);
    assert(depths.empty() && descriptor_types == std::vector<unsigned>{0x29});
    assert(disks.size() == 1 && disks[0]->speed == 3 && addressed[0].ep0[1] >> 16 == 64);
    assert((((volatile uint32_t*)root.device_ctx)[2] >> 16 & 3) == 2);
    for (uint8_t speed : {1, 2}) {
        setup(3); slow_child_speed = speed; enumerate_hub_children(&root, 8, 0);
        assert(keyboards.size() == 1 && keyboards[0]->speed == speed);
        assert(addressed[0].slot[2] == (1u | (1u << 8)));
        assert((((volatile uint32_t*)root.device_ctx)[2] >> 16 & 3) == 2);
    }
    setup(); reject_depth = true; enumerate_hub_children(&root, 8, 0);
    assert(disks.empty() && addressed.empty() && messages.find("SET_HUB_DEPTH failed") != std::string::npos);
    setup(); malformed_descriptor = true; enumerate_hub_children(&root, 8, 0);
    assert(disks.empty() && addressed.empty() && depths.empty());
    // An SSP upstream link does not imply SSP children: a 5-Gbit/s drive keeps 4.
    setup(5); enumerate_hub_children(&root, 8, 0);
    assert(disks.size() == 1 && disks[0]->speed == 4 && (addressed[0].slot[0] >> 20 & 15) == 4);
    // Model the controller's mandatory output-speed update after an SSP device
    // sends its Sublink Speed notification during Address Device.
    setup(5); negotiated_child_speed = 5; enumerate_hub_children(&root, 8, 0);
    assert(disks.size() == 1 && disks[0]->speed == 5 && addressed[0].ep0[1] >> 16 == 512);
    setup(5); negotiated_child_speed = 5; nested = true; enumerate_hub_children(&root, 8, 0);
    assert(disks.size() == 1 && disks[0]->speed == 5 && (depths == std::vector<unsigned>{0, 1}));
    assert((addressed.back().slot[0] & 0xfffff) == 0x11);
    setup(8); enumerate_hub_children(&root, 8, 0);
    assert(disks.empty() && descriptor_types.empty() && messages.find("speed ID") != std::string::npos);
    for (void* p : allocations) std::free(p);
    std::puts("Actual hub enumerator: USB 3 descriptors/depth/route/EP0, nested hubs, USB 2 regression and failure containment passed");
}
