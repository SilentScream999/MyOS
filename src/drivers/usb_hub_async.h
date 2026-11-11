#ifndef WYZAR_USB_HUB_ASYNC_H
#define WYZAR_USB_HUB_ASYNC_H
#include "usb_hub_protocol.h"

// Background hub monitoring only. Enumeration retains its bounded synchronous
// control path. Submit once, then let the input task sleep/process other events.
static void hub_submit_status_request(int h, bool clear, uint16_t feature = 0) {
    USBDevice& hub = *hubs[h];
    HubStatusWatch& watch = hub_watch[h];
    USBHubControl& request = watch.control;
    USBControlGuard guard; // Protect only TRB publication, never completion wait.
    const bool port = watch.scan_port != 0;
    uint8_t request_type = clear ? (port ? 0x23 : 0x20) : (port ? 0xA3 : 0xA0);
    uint16_t length = clear ? 0 : 4;
    for (int b = 0; b < 4; ++b) request.data[b] = 0;
    request.length = request.actual = length;
    request.complete = false;
    request.code = 0;
    request.setup_trb = hub.ep0_ring->phys + hub.ep0_ring->enq * 16u;
    uint64_t setup = request_type | ((uint64_t)(clear ? 1u : 0u) << 8) |
        ((uint64_t)feature << 16) | ((uint64_t)watch.scan_port << 32) |
        ((uint64_t)length << 48);
    ring_push_cmd(hub.ep0_ring, (TRB_TYPE_SETUP_STAGE << 10) | (1u << 6) |
        (setup_trt(request_type, length) << 16), setup, 8);
    request.data_trb = 0;
    if (length) {
        request.data_trb = hub.ep0_ring->phys + hub.ep0_ring->enq * 16u;
        ring_push_cmd(hub.ep0_ring, (TRB_TYPE_DATA_STAGE << 10) | (1u << 16) |
            (1u << 2), (uint64_t)request.data - HHDM, length); // ISP for short reads.
        ++g_usb_hub_status_requests;
    }
    request.status_trb = hub.ep0_ring->phys + hub.ep0_ring->enq * 16u;
    ring_push_cmd(hub.ep0_ring, (TRB_TYPE_STATUS_STAGE << 10) | (1u << 5) |
        (length ? 0u : (1u << 16)), 0, 0);
    request.deadline = _usb_rdtsc() + (_usb_tsc_hz() / 1000) * 250u;
    request.active = true;
    __asm__ volatile("sfence" ::: "memory");
    hub.hc->doorbell32[hub.slot_id] = 1;
}

static bool hub_dispatch_control(XHCIController* hc, const USB_Response& response) {
    if (response.type != 32) return false;
    // Copy fields while the event is owned by this dispatch pass.
    const uint64_t pointer = response.event->parameter;
    const uint32_t status = response.event->status;
    for (int h = 0; h < hub_count; ++h)
        if (hubs[h]->hc == hc && usb_hub_control_event(hub_watch[h].control,
                hubs[h]->slot_id, response.ctrl, pointer, status)) return true;
    return false;
}

static void hub_finish_status_scan(int h) {
    HubStatusWatch& watch = hub_watch[h];
    watch.scan_mask = 0;
    watch.poll.pending |= watch.recheck_mask;
    watch.recheck_mask = 0;
    usb_hub_poll_finish(watch.poll, _usb_rdtsc(), _usb_tsc_hz() / 1000, watch.audited, false);
}

static uint8_t hub_poll_changes(int h) {
    USBDevice& hub = *hubs[h];
    HubStatusWatch& watch = hub_watch[h];
    USBHubControl& request = watch.control;
    if (!watch.ready) return 0;
    if (!watch.scan_mask) {
        watch.scan_mask = usb_hub_poll_plan(watch.poll, watch.num_ports, _usb_rdtsc(), watch.audited);
        if (!watch.scan_mask) { hub_queue_status(h); return 0; }
        watch.phase = 0;
    }
    if (request.active) {
        if (!request.complete && _usb_rdtsc() < request.deadline) return 0;
        bool timeout = !request.complete;
        bool failed = timeout || request.code != 1 || request.actual < request.length;
        request.active = false;
        if (failed) {
            // A timed-out/stalled EP0 must not be reused without endpoint recovery.
            // Keep its buffers alive for late DMA; root removal still cleans up.
            watch.ready = false;
            watch.scan_mask = 0;
            print(timeout ? (char*)"[usb] Hub background status timeout; preserving other devices." :
                            (char*)"[usb] Hub background status failed; preserving other devices.");
            return 0;
        }
        if (watch.phase == 1) {
            watch.sampled_at = _usb_rdtsc();
            for (int b = 0; b < 4; ++b) watch.status[b] = request.data[b];
            if (watch.scan_port) {
                uint8_t port = watch.scan_port;
                xhci_port_note_change(watch.changes[port], watch.ports[port].baseline,
                    watch.status[0] | ((watch.status[2] & 1u) ? (1u << 17) : 0u), watch.sampled_at);
                watch.clear_bits = (watch.status[2] | (watch.status[3] << 8)) & usb_hub_change_mask(hub.speed);
            } else watch.clear_bits = watch.status[2] & 3u;
            watch.phase = 2;
        }
    }
    if (watch.phase == 2) {
        if (watch.clear_bits) {
            uint8_t bit = 0;
            while (!(watch.clear_bits & (1u << bit))) ++bit;
            watch.clear_bits &= ~(1u << bit);
            uint16_t feature = watch.scan_port ? usb_hub_change_feature(hub.speed, bit) : bit;
            hub_submit_status_request(h, true, feature);
            return 0;
        }
        uint8_t port = watch.scan_port;
        watch.scan_mask &= ~(1u << port);
        if (port) {
            uint64_t settle = (_usb_tsc_hz() / 1000) * 150u;
            if (xhci_port_watch_changed(watch.ports[port], watch.status[0], watch.sampled_at, settle) ||
                xhci_port_replaced(watch.changes[port], watch.status[0], watch.sampled_at, settle)) {
                char detail[32];
                print((char*)"[usb] Stable hub port change; slot:"); to_str(hub.slot_id, detail); print(detail);
                print((char*)"port:"); to_str(port, detail); print(detail);
                hub_finish_status_scan(h);
                return port;
            }
            if (xhci_port_presence(watch.status[0]) != watch.ports[port].baseline ||
                (watch.changes[port].replacement && (watch.status[0] & 1u))) watch.poll.pending |= 1u << port;
            else watch.poll.pending &= ~(1u << port);
        } else watch.poll.pending &= ~1u;
        watch.phase = 0;
        if (!watch.scan_mask) {
            watch.failures = 0;
            hub_finish_status_scan(h);
            hub_queue_status(h);
            return 0;
        }
    }
    // At most one new control transaction is submitted per input pass.
    watch.scan_port = 0;
    while (!(watch.scan_mask & (1u << watch.scan_port))) ++watch.scan_port;
    watch.phase = 1;
    hub_submit_status_request(h, false);
    return 0;
}
#endif
