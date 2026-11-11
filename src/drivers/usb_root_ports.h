#ifndef WYZAR_USB_ROOT_PORTS_H
#define WYZAR_USB_ROOT_PORTS_H

// Included after the hub/HID helpers. Root hotplug must not reset xHCI or
// replace existing block_device objects: only the affected port owns this work.
static bool usb_root_command(XHCIController* hc, uint32_t control, uint64_t parameter,
                             uint8_t* returned_slot = nullptr) {
    if (usb_probe_expired(hc)) return false;
    ring_push_cmd(&hc->cr, control, parameter, 0);
    __asm__ volatile("sfence" ::: "memory");
    hc->doorbell32[0] = 0;
    uint64_t end = usb_timeout_deadline(hc, 1000);
    do {
        USB_Response r = get_usb_command_response(hc, 1);
        if (!r.gotresponse) continue;
        if (returned_slot) *returned_slot = (r.ctrl >> 24) & 255;
        if ((r.event->status >> 24) == 1) return true;
        char detail[32];
        print((char*)"[usb] Root command failed, completion:");
        to_str(r.event->status >> 24, detail); print(detail);
        return false;
    } while (_usb_rdtsc() < end);
    print((char*)"[usb] Root command timed out; other ports retained.");
    usb_probe_state[hc - xhci_hc].timed_out = true;
    return false;
}

static volatile uint32_t* usb_root_portsc(XHCIController* hc, uint32_t port) {
    return (volatile uint32_t*)((uintptr_t)hc->ops + 0x400 + port * 0x10);
}

static void usb_root_ack_changes(XHCIController* hc, uint32_t port, uint32_t sc, uint64_t now) {
    int c = hc - xhci_hc;
    xhci_port_note_change(usb_root_changes[c][port], usb_root_watch[c][port].baseline, sc, now);
    if (sc & (127u << 17)) *usb_root_portsc(hc, port) = xhci_port_ack(sc);
}

static bool usb_root_reset(XHCIController* hc, uint32_t port) {
    volatile uint32_t* portsc = usb_root_portsc(hc, port);
    uint32_t sc = *portsc;
    if (!xhci_port_probe_needed(sc)) return false;
    // Do not restart an already-running reset. Allow PED to settle too; PR
    // clearing before PED rises must not permanently blacklist the port.
    if (!(sc & ((1u << 4) | (1u << 31))))
        *portsc = xhci_port_ack(sc) | xhci_port_reset_bit(sc);
    uint64_t end = usb_timeout_deadline(hc, 500);
    do {
        sc = *portsc;
        if (!xhci_port_probe_needed(sc) && !(sc & ((1u << 4) | (1u << 31)))) return false;
        if (!(sc & ((1u << 4) | (1u << 31))) && (sc & 3u) == 3u) {
            tsc_delay_ms(10);
            if (*portsc & (127u << 17)) *portsc = xhci_port_ack(*portsc);
            return true;
        }
        tsc_delay_ms(1);
    } while (_usb_rdtsc() < end);
    char detail[32];
    print((char*)"[usb] Root reset not ready; port:"); to_str(port + 1, detail); print(detail);
    print((char*)"PORTSC:"); to_hex(*portsc, detail); print(detail);
    return false;
}

#include "usb_device_tree.h"

static void usb_detach_branch(XHCIController* hc, uint8_t root, USBDevice* hub = nullptr, uint8_t port = 0) {
    int keep = 0;
    for (int k = 0; k < keyboard_count; ++k)
        if (keyboards[k]->hc != hc || !usb_in_branch(keyboards[k], root, hub, port))
            keyboards[keep++] = keyboards[k];
    keyboard_count = keep;
    keep = 0;
    for (int m = 0; m < mouse_count; ++m)
        if (mice[m]->hc != hc || !usb_in_branch(mice[m], root, hub, port))
            mice[keep++] = mice[m];
    mouse_count = keep;
    keep = 0;
    for (int h = 0; h < hub_count; ++h) {
        if (hubs[h]->hc == hc && usb_in_branch(hubs[h], root, hub, port)) continue;
        if (keep != h) {
            hubs[keep] = hubs[h]; hub_dcis[keep] = hub_dcis[h];
            for (size_t b = 0; b < sizeof(HubStatusWatch); ++b)
                ((volatile uint8_t*)&hub_watch[keep])[b] = ((volatile uint8_t*)&hub_watch[h])[b];
        }
        ++keep;
    }
    hub_count = keep;
    for (int d = 0; d < usb_disk_count; ++d)
        if (usb_disks[d].dev->hc == hc && usb_in_branch(usb_disks[d].dev, root, hub, port))
            usb_disks[d].disk.online = false;
    int c = hc - xhci_hc;
    for (int slot = 255; slot > 0; --slot) {
        USBDevice* dev = usb_slot_devices[c][slot];
        if (!dev || !usb_in_branch(dev, root, hub, port)) continue;
        // Disable Slot stops all endpoints. Retain their allocations so late
        // events/DMA can never reference freed memory, even on command timeout.
        if (usb_root_command(hc, (10u << 10) | ((uint32_t)slot << 24), 0)) hc->dcbaa[slot] = 0;
        usb_slot_devices[c][slot] = nullptr;
    }
}

static void usb_root_detach(XHCIController* hc, uint32_t port) {
    usb_detach_branch(hc, port + 1);
    usb_root_devices[hc - xhci_hc][port] = nullptr;
}

static void usb_hub_reconcile(USBDevice* hub, uint8_t port) {
    USBControlGuard guard;
    USBProbeGuard probe(hub->hc);
    char detail[32];
    print((char*)"[usb] Hub port change handled locally; slot:"); to_str(hub->slot_id, detail); print(detail);
    print((char*)"port:"); to_str(port, detail); print(detail);
    usb_detach_branch(hub->hc, 0, hub, port);
    uint8_t status[4] = {};
    if (!usb_probe_expired(hub->hc) && hub_get_port_status(*hub, port, status) && (status[0] & 1u)) {
        hub->hc->global_port_index = 0;
        if (hub->hc->portfailed) for (int p = 0; p < 4096; ++p) hub->hc->portfailed[p] = false;
        enumerate_hub_children(hub, 8, port);
    }
    if (hub->hc->needsResetting || usb_probe_expired(hub->hc) || usb_probe_state[hub->hc - xhci_hc].timed_out) {
        hub->hc->needsResetting = false;
        USBProbeCleanup cleanup(hub->hc);
        usb_detach_branch(hub->hc, 0, hub, port);
    }
    // Removing a nested hub may compact the registration table; find the parent
    // again. Its armed interrupt/report buffer and all sibling devices survive.
    for (int h = 0; h < hub_count; ++h) if (hubs[h] == hub) {
        HubStatusWatch& watch = hub_watch[h];
        if (!usb_probe_expired(hub->hc) && hub_get_port_status(*hub, port, status)) hub_ack_port_changes(*hub, port, status);
        xhci_port_watch_init(watch.ports[port], status[0]);
        watch.changes[port].replacement = false;
        watch.poll.pending &= ~(1u << port);
        watch.failures = 0; watch.poll.pending_after = 0;
        hub_queue_status(h);
        break;
    }
}

static void usb_root_enumerate(XHCIController* hc, uint32_t port, bool reset) {
    USBControlGuard guard;
    USBProbeGuard probe(hc);
    if (usb_probe_expired(hc)) return;
    int c = hc - xhci_hc;
    volatile uint32_t* portsc = usb_root_portsc(hc, port);
    uint32_t sc = *portsc;
    usb_root_attempted[c][port] = true;
    if (!xhci_port_probe_needed(sc)) return;
    if (reset && !usb_root_reset(hc, port)) {
        // A later PED transition is a chance to enumerate this port locally,
        // not an invitation to reset the entire controller again.
        usb_root_attempted[c][port] = false;
        return;
    }
    sc = *portsc;
    if ((sc & 3u) != 3u || (sc & ((1u << 4) | (1u << 31)))) return;
    uint32_t speed = (sc >> 10) & 15u;
    char detail[32];
    print((char*)"Device on root port:"); to_str(port + 1, detail); print(detail);
    print((char*)"Speed:"); to_str(speed, detail); print(detail);
    uint8_t slot = 0;
    if (!usb_root_command(hc, 9u << 10, 0, &slot) || !slot) return;
    USBDevice* dev = (USBDevice*)alloc_table();
    dev->hc = hc; dev->slot_id = slot; dev->port_num = port + 1;
    dev->root_port_num = port + 1; dev->speed = speed;
    usb_slot_devices[c][slot] = dev;
    dev->ep0_ring = (Ring*)alloc_table(); ring_init(dev->ep0_ring);
    dev->ep0_ring_phys = dev->ep0_ring->phys;
    dev->device_ctx = alloc_table();
    dev->dev_ctx_phys = (uint64_t)dev->device_ctx - HHDM;
    hc->dcbaa[slot] = dev->dev_ctx_phys;
    size_t stride = (hc->hcc1 & 4u) ? 64u : 32u;
    volatile uint64_t* input = alloc_table();
    for (int j = 0; j < 512; ++j) input[j] = 0;
    ((volatile InputControlCtx*)input)->add_flags = 3;
    volatile uint32_t* slot_ctx = (volatile uint32_t*)((uint64_t)input + stride);
    volatile uint32_t* ep0 = (volatile uint32_t*)((uint64_t)input + stride * 2);
    slot_ctx[0] = (speed << 20) | (1u << 27);
    slot_ctx[1] = (port + 1) << 16;
    uint16_t mps = speed >= 4 ? 512 : (speed == 3 ? 64 : 8);
    ep0[1] = (3u << 1) | (4u << 3) | ((uint32_t)mps << 16);
    uint64_t dequeue = dev->ep0_ring->phys | 1u;
    ep0[2] = dequeue; ep0[3] = dequeue >> 32; ep0[4] = 8;
    if (!usb_root_command(hc, (11u << 10) | ((uint32_t)slot << 24), (uint64_t)input - HHDM)) {
        usb_root_detach(hc, port); return;
    }
    print((char*)"Address Device success.");
    tsc_delay_ms(10);
    enumerate_device_after_address(dev, nullptr, 0, 0);
    bool input_configured = false;
    for (int i = 0; i < dev->interface_count; ++i) if (dev->interfaces[i].configured) input_configured = true;
    if (hc->needsResetting || (!dev->is_hub && !input_configured && !usb_storage_is_device(dev))) {
        hc->needsResetting = false;
        print((char*)"[usb] Root device unavailable; preserving other ports.");
        USBProbeCleanup cleanup(hc);
        usb_root_detach(hc, port); return;
    }
    usb_root_devices[c][port] = dev;
    if (dev->is_hub) {
        enumerate_hub_children(dev, 8);
        if (hc->needsResetting || usb_probe_expired(hc) || usb_probe_state[c].timed_out) {
            hc->needsResetting = false;
            print((char*)"[usb] Failed hub probe; stopping this root port's slots.");
            USBProbeCleanup cleanup(hc);
            usb_root_detach(hc, port);
        }
    }
}

// Returns true when input lists may have changed. The caller rearms only new
// interfaces; no controller reset, disk rebind, or discard of live events.
static bool usb_root_poll(XHCIController* hc, uint32_t port, uint64_t now) {
    int c = hc - xhci_hc;
    volatile uint32_t* reg = usb_root_portsc(hc, port);
    uint32_t sc = *reg;
    usb_root_ack_changes(hc, port, sc, now);
    uint64_t settle = (_usb_tsc_hz() / 1000) * 150u;
    bool changed = xhci_port_watch_changed(usb_root_watch[c][port], sc, now, settle);
    bool replaced = xhci_port_replaced(usb_root_changes[c][port], sc, now, settle);
    bool late_ready = !usb_root_devices[c][port] && !usb_root_attempted[c][port] &&
        xhci_port_watch_changed(usb_root_ready_watch[c][port],
                               (((sc & 3u) == 3u || (sc & (1u << 24))) &&
                                !(sc & ((1u << 4) | (1u << 31)))) ? 1u : 0u,
                               now, settle);
    if (!changed && !replaced && !late_ready) return false;
    USBControlGuard guard;
    USBProbeGuard probe(hc);
    char detail[32];
    print(replaced ? (char*)"[usb] Root device replacement; port:" :
          late_ready ? (char*)"[usb] Late-ready root port:" : (char*)"[usb] Root connection change; port:");
    to_str(port + 1, detail); print(detail);
    print((char*)"PORTSC:"); to_hex(sc, detail); print(detail);
    usb_root_changes[c][port].replacement = false;
    if (changed || replaced) usb_root_detach(hc, port);
    if (xhci_port_probe_needed(sc)) {
        // Clear only transient enumeration failure bookkeeping, not live slots.
        hc->global_port_index = 0;
        if (hc->portfailed) for (int p = 0; p < 4096; ++p) hc->portfailed[p] = false;
        usb_root_enumerate(hc, port, replaced || !late_ready || xhci_port_reset_bit(sc) == (1u << 31));
    } else usb_root_attempted[c][port] = false;
    xhci_port_watch_init(usb_root_watch[c][port], *reg);
    xhci_port_watch_init(usb_root_ready_watch[c][port], 0);
    return true;
}
#endif
