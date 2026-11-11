#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include "src/core/structures.h"
#include "src/drivers/usb_hub_watch.h"

static constexpr uint64_t HHDM = 0;
static uint64_t now, g_usb_hub_status_requests;
static uint64_t _usb_rdtsc() { return now; }
static uint64_t _usb_tsc_hz() { return 1000; }
struct USBControlGuard {};
static USBDevice* hubs[2];
static int hub_count;
static HubStatusWatch hub_watch[2];
static std::string messages;
static unsigned queued, submissions;
static void print(char* s) { messages += s; }
static void to_str(uint64_t, char* s) { s[0] = 0; }
static void hub_queue_status(int h) {
    if (hub_watch[h].ready && !hub_watch[h].scan_mask) ++queued;
}
static uint32_t setup_trt(uint8_t type, uint16_t length) { return !length ? 0 : (type & 0x80 ? 3 : 2); }
static void ring_push_cmd(Ring* r, uint32_t control, uint64_t parameter, uint32_t status) {
    r->trb[r->enq].control = control;
    r->trb[r->enq].parameter = parameter;
    r->trb[r->enq].status = status;
    if (++r->enq == 255) r->enq = 0;
    ++submissions;
}
#include "src/drivers/usb_hub_async.h"

static XHCIController controller, other_controller;
static USBDevice device;
static Ring ep0;
static TRB trbs[256];
static uint8_t data[4];
static uint32_t doorbells[256];
static void reset() {
    hub_watch[0] = {}; controller = {}; device = {}; ep0 = {};
    std::memset(trbs, 0, sizeof(trbs));
    device.hc = &controller; device.ep0_ring = &ep0; device.slot_id = 7; device.speed = 3;
    controller.doorbell32 = doorbells;
    ep0.trb = trbs; ep0.phys = 0x1000;
    hubs[0] = &device; hub_count = 1;
    HubStatusWatch& w = hub_watch[0];
    w.control.data = data; w.num_ports = 8; w.ready = true;
    w.poll.cursor = 1; w.poll.audit_after = 250;
    now = 0; queued = submissions = g_usb_hub_status_requests = 0; messages.clear();
}
static bool event(uint64_t pointer, uint32_t code = 1, uint32_t residual = 0,
                  uint8_t slot = 7, uint8_t dci = 1, XHCIController* hc = &controller) {
    TRB trb = {}; trb.parameter = pointer; trb.status = (code << 24) | residual;
    USB_Response response = {}; response.type = 32; response.ctrl = ((uint32_t)slot << 24) | ((uint32_t)dci << 16);
    response.event = &trb;
    return hub_dispatch_control(hc, response);
}
static void status(uint8_t presence = 0, uint8_t change = 0) {
    data[0] = presence; data[1] = 0; data[2] = change; data[3] = 0;
    assert(event(hub_watch[0].control.status_trb));
}
int main() {
    reset(); auto& w = hub_watch[0];
    now = 250; assert(hub_poll_changes(0) == 0);
    assert(w.control.active && !w.control.complete && submissions == 3);
    assert((trbs[0].parameter & 0xff) == 0xA3 && ((trbs[0].parameter >> 32) & 65535) == 1);
    assert(doorbells[7] == 1 && g_usb_hub_status_requests == 1);
    // Repeated passes with missing completion neither spin nor submit duplicates.
    for (int i = 0; i < 1000; ++i) assert(hub_poll_changes(0) == 0);
    assert(submissions == 3 && !w.control.complete);
    assert(!event(w.control.status_trb, 1, 0, 8));
    assert(!event(w.control.status_trb, 1, 0, 7, 3));
    assert(!event(w.control.status_trb, 1, 0, 7, 1, &other_controller));
    assert(!event(w.control.status_trb + 16));
    // Short data is not final; a copied result survives the stack event's lifetime.
    assert(event(w.control.data_trb, 13, 1)); assert(!w.control.complete && w.control.actual == 3);
    status(); now = 258; assert(hub_poll_changes(0) == 0);
    assert(!w.ready && messages.find("failed") != std::string::npos);

    reset(); now = 250; hub_poll_changes(0);
    // Latched connection changes are acknowledged asynchronously, before rearm.
    status(1, 1); now = 258; hub_poll_changes(0);
    assert(w.control.active && w.control.length == 0 && w.phase == 2);
    uint64_t setup = trbs[3].parameter;
    assert((setup & 0xffff) == 0x123 && ((setup >> 16) & 65535) == 16);
    assert(queued == 0 && w.changes[1].replacement == false);
    assert(event(w.control.status_trb)); now = 266; hub_poll_changes(0);
    assert(!w.control.active && queued == 1 && (w.poll.pending & 2));
    assert(w.poll.pending_after == 316 && w.poll.audit_after == 516);
    now = 316; hub_poll_changes(0); status(1); now = 324; hub_poll_changes(0);
    now = 424; hub_poll_changes(0); status(1); now = 432;
    assert(hub_poll_changes(0) == 1 && !w.control.active && !w.scan_mask);

    reset(); now = 250; hub_poll_changes(0);
    now = 499; hub_poll_changes(0); assert(w.ready);
    now = 500; hub_poll_changes(0);
    assert(!w.ready && !w.control.active && submissions == 3);
    assert(messages.find("timeout") != std::string::npos);
    assert(!event(w.control.status_trb)); // Late DMA/completion cannot revive it.
    assert(!controller.needsResetting);

    reset(); w.poll.pending = 1; w.poll.audit_after = 1000;
    hub_poll_changes(0); assert(w.scan_port == 0);
    status(0, 3); hub_poll_changes(0); // Hub local-power change clear.
    assert(((trbs[3].parameter >> 16) & 65535) == 0);
    assert(event(w.control.status_trb)); hub_poll_changes(0); // Over-current clear.
    assert(((trbs[5].parameter >> 16) & 65535) == 1);
    assert(event(w.control.status_trb)); hub_poll_changes(0);
    assert(!w.poll.pending && !w.scan_mask && queued == 1);

    reset(); w.poll.pending = (1u << 1) | (1u << 2); w.poll.audit_after = 1000;
    hub_poll_changes(0); status(); hub_poll_changes(0);
    assert(w.scan_port == 2 && w.control.active);
    // A notification arriving during a scan must remain scheduled after it.
    w.recheck_mask = 1u << 1;
    status(); hub_poll_changes(0);
    assert(w.poll.pending == (1u << 1) && !w.scan_mask);

    reset(); ep0.enq = 254; now = 250; hub_poll_changes(0);
    assert(w.control.setup_trb == 0x1fe0 && w.control.data_trb == 0x1000 && w.control.status_trb == 0x1010);
    assert(event(w.control.data_trb, 6)); // Error before status stage is terminal.
    hub_poll_changes(0); assert(!w.ready && !controller.needsResetting);
    // SS change bits 5..7 have different selectors from USB 2's L1 bit.
    for (auto pair : {std::pair<uint8_t, unsigned>{5, 29}, {6, 25}, {7, 26}}) {
        reset(); device.speed = 4; now = 250; hub_poll_changes(0);
        status(3, 1u << pair.first); now = 258; hub_poll_changes(0);
        assert(w.control.active && w.control.length == 0);
        assert(((trbs[3].parameter >> 16) & 65535) == pair.second);
        assert(event(w.control.status_trb)); hub_poll_changes(0);
        assert(w.ready && queued == 1);
    }
    std::puts("Async hub monitor: completion ownership, USB 2/3 acknowledgments, debounce, timeout, late events and ring wrap passed");
}
