// src/usbhelpers_fixed.h
#ifndef usbhelp_h
#define usbhelp_h

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "scheduler.h"

#include "structures.h"
#include "pagingstuff.h"
#include "helpers.h"
#include "block.h"
#include "xhci_port_state.h"
#include "usb_hub_poll_policy.h"
#include "usb_hub_transfer.h"
#include "usb_hub_protocol.h"
#include "usbhelpers.h"

/* ── Ring / Event Ring sizes ──────────────────────────────── */
#define RING_SIZE   256
#define LINK_INDEX  (RING_SIZE - 1)

#define ER_RING_SIZE   256
#define ER_LINK_INDEX  (ER_RING_SIZE - 1)

#ifndef TRB_TYPE_LINK
#define TRB_TYPE_LINK 6
#endif

/* ── Module-level globals ─────────────────────────────────── */
#define MAX_CONTROLLERS 4
static XHCIController xhci_hc[MAX_CONTROLLERS];
static int xhci_count = 0;
static uint64_t g_usb_hub_status_requests = 0;
static XHCIPortWatch usb_root_watch[MAX_CONTROLLERS][256] = {};
static XHCIPortWatch usb_root_ready_watch[MAX_CONTROLLERS][256] = {};
static XHCIPortChangeLatch usb_root_changes[MAX_CONTROLLERS][256] = {};
static USBDevice* usb_root_devices[MAX_CONTROLLERS][256] = {};
static USBDevice* usb_slot_devices[MAX_CONTROLLERS][256] = {};
static bool usb_root_attempted[MAX_CONTROLLERS][256] = {};

// ── TSC-based delay helpers ───────────────────────────────────────────────────
// Mirrors the timing strategy in kmain.cpp: use the invariant TSC so delays
// are correct regardless of loop speed or compiler optimisation level.
// All spin_delay() calls in this file are replaced with tsc_delay_ms().

static inline uint64_t _usb_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t _usb_tsc_hz(void) {
    uint32_t eax, ebx, ecx, edx;
    // CPUID leaf 0x15: TSC/crystal ratio (Intel Skylake+)
    __asm__ volatile ("cpuid"
        : "=a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx)
        : "a"(0x15u) : "memory");
    if (eax && ebx && ecx) return (uint64_t)ecx * ebx / eax;
    // CPUID leaf 0x16: base frequency in MHz
    __asm__ volatile ("cpuid"
        : "=a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx)
        : "a"(0x16u) : "memory");
    if (eax & 0xFFFFu) return (uint64_t)(eax & 0xFFFFu) * 1000000ULL;
    return 3000000000ULL;   // fallback: 3 GHz
}

// Accurate millisecond delay — call from anywhere in usbhelpers.h
#include "usb_probe_budget.h"

static void tsc_delay_ms(uint64_t ms) {
    uint64_t irq_flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(irq_flags));
    if (g_scheduler_ready && (irq_flags & (1u << 9))) {
        task_sleep_ms(ms);
        return;
    }
	uint64_t hz = _usb_tsc_hz();
	uint64_t start = _usb_rdtsc();
	uint64_t end = start + (hz / 1000) * ms;
	while (_usb_rdtsc() < end) {
		boot_splash_tick();
		__asm__ volatile ("pause");
	}
}

/* ── Ring helpers ─────────────────────────────────────────── */
static void ring_init(struct Ring *r) {
	r->trb  = (volatile TRB*)alloc_table();
	r->phys = (uint64_t)r->trb - HHDM;
	r->enq  = 0;
	r->pcs  = 1;

	for (int i = 0; i < RING_SIZE; i++) r->trb[i].control = 0;

	r->trb[LINK_INDEX].parameter = r->phys;
	r->trb[LINK_INDEX].status    = 0;
	r->trb[LINK_INDEX].control   = (TRB_TYPE_LINK << 10) | (1u << 1) | (r->pcs & 1u);
}

static void ring_push_cmd(struct Ring *r, uint32_t ctrl, uint64_t param, uint32_t status) {
	volatile TRB *t = &r->trb[r->enq];
	t->parameter = param;
	t->status    = status;
	t->control   = (ctrl & ~1u) | (r->pcs & 1u);

	if (++r->enq == LINK_INDEX) {
		// Write link TRB with the OLD (current) pcs so the controller can
		// cross it while still in the old cycle, THEN flip pcs for new TRBs.
		// Writing new pcs first causes the controller to see the wrong cycle
		// bit, mistake the ring for empty, and stop after 255 entries.
		r->trb[LINK_INDEX].control = (TRB_TYPE_LINK << 10) | (1u << 1) | (r->pcs & 1u);
		r->enq = 0;
		r->pcs ^= 1;
	}
}

/* ── get_usb_response ─────────────────────────────────────── */
static USB_Response get_usb_response_raw(XHCIController* hc, int timeout_ms = 250) {
    uint64_t end = _usb_rdtsc() + (_usb_tsc_hz() / 1000) * (uint64_t)timeout_ms;

    for (;;) {
        TRB* evt  = (TRB*)&hc->er_virt[hc->erdp_index];
        uint32_t ctrl = evt->control;

        if ((ctrl & 1u) == hc->ccs) {
            // Event is ready
            hc->erdp_index = (hc->erdp_index + 1) % ER_RING_SIZE;
            if (hc->erdp_index == 0) hc->ccs ^= 1;
            *hc->erdp = (hc->er_phys + (uint64_t)hc->erdp_index * 16u) | (1ull << 3);

            USB_Response resp;
            resp.gotresponse = true;
            resp.event       = evt;
            resp.type        = (ctrl >> 10) & 0x3F;
            resp.ctrl        = ctrl;
            return resp;
        }

        // Timeout check happens after at least one attempt
        if (_usb_rdtsc() >= end) {
            USB_Response resp;
            resp.gotresponse = false;
            return resp;
        }

        __asm__ volatile ("pause");
    }
}

// Synchronous disk I/O shares the event ring with HID and hotplug. Keep copies
// of unrelated events until the regular polling loop can dispatch them.
struct usb_deferred_events {
    TRB events[256];
    uint16_t head, count;
    TRB delivered;
};
static usb_deferred_events usb_pending[MAX_CONTROLLERS] = {};
static usb_deferred_events* usb_pending_for(XHCIController* hc) { return &usb_pending[hc - xhci_hc]; }
static bool usb_defer(XHCIController* hc, const USB_Response& r) {
    usb_deferred_events* q = usb_pending_for(hc);
    if (q->count == 256) { hc->needsResetting = true; return false; }
    TRB* e = &q->events[(q->head + q->count++) % 256];
    e->parameter = r.event->parameter; e->status = r.event->status; e->control = r.ctrl;
    return true;
}
static USB_Response get_usb_response(XHCIController* hc, int timeout_ms = 250) {
    usb_deferred_events* q = usb_pending_for(hc);
    if (!q->count) return get_usb_response_raw(hc, timeout_ms);
    TRB* e = &q->events[q->head];
    q->delivered.parameter = e->parameter; q->delivered.status = e->status; q->delivered.control = e->control;
    q->head = (q->head + 1) % 256; --q->count;
    USB_Response r;
    r.gotresponse = true; r.event = &q->delivered; r.ctrl = q->delivered.control; r.type = (r.ctrl >> 10) & 63;
    return r;
}

/* ── do_control_transfer ──────────────────────────────────── */
// One synchronous command is outstanding per controller. Match its exact TRB
// while keeping live HID/status completions for the regular dispatcher.
static USB_Response get_usb_command_response(XHCIController* hc, int timeout_ms = 10) {
    uint16_t previous = hc->cr.enq ? hc->cr.enq - 1 : LINK_INDEX - 1;
    uint64_t pointer = hc->cr.phys + previous * 16u;
    uint64_t end = usb_timeout_deadline(hc, timeout_ms);
    do {
        USB_Response r = get_usb_response_raw(hc, 0);
        if (r.gotresponse) {
            if (r.type == 33 && r.event->parameter == pointer) return r;
            if (!usb_defer(hc, r)) break;
        }
        __asm__ volatile("pause");
    } while (_usb_rdtsc() < end);
    USB_Response result = {};
    return result;
}

static USB_Response usb_wait_command(XHCIController* hc, const char* operation) {
    USB_Response response = get_usb_command_response(hc, 1000);
    if (!response.gotresponse) {
        usb_probe_state[hc - xhci_hc].timed_out = true;
        print((char*)"[usb] Command completion timeout:"); print((char*)operation);
        usb_probe_expired(hc);
    }
    return response;
}

struct USBControlGuard {
    uint64_t flags;
    USBControlGuard() { __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory"); }
    ~USBControlGuard() { if (flags & (1u << 9)) __asm__ volatile("sti" ::: "memory"); }
};
static USB_Response do_control_transfer(USBDevice &dev,
										USBSetupPacket* setup,
										volatile uint8_t* data_buffer,
										uint16_t data_len) {
	USBControlGuard guard;
	if (usb_probe_expired(dev.hc)) { USB_Response empty = {}; return empty; }
	uint64_t setup_pointer = dev.ep0_ring->phys + dev.ep0_ring->enq * 16u;
	uint64_t data_pointer = 0;
	uint64_t setup_dw0 =
		((uint64_t)setup->bmRequestType)    |
		((uint64_t)setup->bRequest   <<  8) |
		((uint64_t)setup->wValue     << 16) |
		((uint64_t)setup->wIndex     << 32) |
		((uint64_t)setup->wLength    << 48);

	ring_push_cmd(dev.ep0_ring,
		(TRB_TYPE_SETUP_STAGE << 10) | (1u << 6) |
		(setup_trt(setup->bmRequestType, setup->wLength) << 16),
		setup_dw0, 8u);

	const bool has_data   = (data_len > 0);
	const bool data_is_in = (setup->bmRequestType & 0x80u) != 0;

	if (has_data) {
		data_pointer = dev.ep0_ring->phys + dev.ep0_ring->enq * 16u;
		uint64_t data_phys = (uint64_t)data_buffer - HHDM;
		ring_push_cmd(dev.ep0_ring,
			(TRB_TYPE_DATA_STAGE << 10) | (data_is_in ? (1u << 16) : 0u),
			data_phys, (uint32_t)data_len);
	}

	uint32_t status_dir = data_is_in ? 0u : (1u << 16);
	uint64_t status_pointer = dev.ep0_ring->phys + dev.ep0_ring->enq * 16u;
	ring_push_cmd(dev.ep0_ring,
		(TRB_TYPE_STATUS_STAGE << 10) | status_dir | (1u << 5),
		0, 0);

	__asm__ volatile("sfence" ::: "memory");
	dev.hc->doorbell32[dev.slot_id] = 1;

	USB_Response resp;
	resp.gotresponse = false;
	
	uint64_t hz = _usb_tsc_hz();
	uint64_t start_time = _usb_rdtsc();
	uint64_t end_time = usb_timeout_deadline(dev.hc, 250);

	while (_usb_rdtsc() < end_time) {
		// Poll hardware, leaving deferred HID/PSC events for the dispatcher.
		resp = get_usb_response_raw(dev.hc, 1);
		if (!resp.gotresponse) continue;
		bool ours = resp.type == 32 && ((resp.ctrl >> 24) & 255) == dev.slot_id &&
		            ((resp.ctrl >> 16) & 31) == 1 &&
		            (resp.event->parameter == setup_pointer ||
		             (has_data && resp.event->parameter == data_pointer) ||
		             resp.event->parameter == status_pointer);
		if (!ours) {
			if (!usb_defer(dev.hc, resp)) break;
			continue;
		}
		uint32_t code = resp.event->status >> 24;
		if (code != 1 && code != 13) return resp;
		// A short data-stage packet does not finish the control transaction.
		if (resp.event->parameter == status_pointer) return resp;
	}

	resp.gotresponse = false;
	usb_probe_state[dev.hc - xhci_hc].timed_out = true;
	return resp;
}

static bool usb_control_ok(const USB_Response& r) {
    return r.gotresponse && (r.event->status >> 24) == 1;
}

#include "usb_storage.h"

/* ── Descriptor helpers ───────────────────────────────────── */
static bool get_device_descriptor(USBDevice &dev, volatile uint8_t* outbuf, uint16_t len) {
	volatile uint8_t* buf = (volatile uint8_t*)alloc_table();
	for (int i = 0; i < 4096; i++) buf[i] = 0;

	USBSetupPacket setup;
	setup.bmRequestType = 0x80;
	setup.bRequest      = 0x06;
	setup.wValue        = (0x01u << 8);
	setup.wIndex        = 0;
	setup.wLength       = len;

	USB_Response r = do_control_transfer(dev, &setup, buf, len);
	if (!usb_control_ok(r)) return false;
	for (int i = 0; i < len; i++) outbuf[i] = buf[i];
	return true;
}

static bool get_configuration_descriptor(USBDevice &dev, volatile uint8_t* outbuf, uint16_t maxlen) {
	volatile uint8_t* buf = (volatile uint8_t*)alloc_table();
	for (int i = 0; i < 4096; i++) buf[i] = 0;

	USBSetupPacket setup;
	setup.bmRequestType = 0x80;
	setup.bRequest      = 0x06;
	setup.wValue        = (0x02u << 8);
	setup.wIndex        = 0;
	setup.wLength       = maxlen;

	USB_Response r = do_control_transfer(dev, &setup, buf, maxlen);
	if (!usb_control_ok(r)) { print((char*)"cfg: transfer failed"); return false; }

	uint16_t total = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
	if (total == 0) { print((char*)"cfg: wTotalLength=0"); return false; }
	if (total > maxlen) total = maxlen;

	for (int i = 0; i < total; i++) outbuf[i] = buf[i];
	return true;
}

/* ── Hub class helpers ────────────────────────────────────── */
#include "logitech_g502.h"
static bool hub_set_port_feature(USBDevice &hub, uint16_t port, uint16_t feature) {

	USBSetupPacket setup;
	setup.bmRequestType = 0x23;
	setup.bRequest      = 0x03;   // SET_FEATURE
	setup.wValue        = feature;
	setup.wIndex        = port;
	setup.wLength       = 0;

	USB_Response r = do_control_transfer(hub, &setup, nullptr, 0);
	return usb_control_ok(r);
}

static bool hub_clear_port_feature(USBDevice &hub, uint16_t port, uint16_t feature) {

	USBSetupPacket setup;
	setup.bmRequestType = 0x23;
	setup.bRequest      = 0x01;   // CLEAR_FEATURE
	setup.wValue        = feature;
	setup.wIndex        = port;
	setup.wLength       = 0;

	USB_Response r = do_control_transfer(hub, &setup, nullptr, 0);
	return usb_control_ok(r);
}

static bool hub_get_port_status(USBDevice &hub, uint16_t port, uint8_t* status4) {
	++g_usb_hub_status_requests;
	USBControlGuard guard;
	static volatile uint8_t* scratch[MAX_CONTROLLERS] = {};
	volatile uint8_t*& tmp = scratch[hub.hc - xhci_hc];
	if (!tmp) tmp = (volatile uint8_t*)alloc_table();
	for (int i = 0; i < 4; i++) tmp[i] = 0;

	USBSetupPacket setup;
	setup.bmRequestType = 0xA3;
	setup.bRequest      = 0x00;   // GET_STATUS
	setup.wValue        = 0;
	setup.wIndex        = port;
	setup.wLength       = 4;

	USB_Response r = do_control_transfer(hub, &setup, tmp, 4);
	if (!usb_control_ok(r)) return false;
	for (int i = 0; i < 4; i++) status4[i] = tmp[i];
	return true;
}

/* ── Keyboard / mouse / hub arrays ────────────────────────── */
#define MAX_KEYBOARDS 8
static  USBDevice*    keyboards[MAX_KEYBOARDS];
static  int           keyboard_count = 0;

#define MAX_MICE 8
static  USBDevice*    mice[MAX_MICE];
static  int           mouse_count = 0;

// Each registered hub gets a slot here so kmain can queue and service
// its status-change interrupt endpoint — exactly like Linux hub_activate()
// and hub_irq().  hub_dcis[] stores the DCI of each hub's interrupt IN
// endpoint so the doorbell can be rung with the right value.
#define MAX_HUBS 4
static  USBDevice*    hubs[MAX_HUBS];
static  uint8_t       hub_dcis[MAX_HUBS];   // DCI = ep_num*2+1 for IN
static  int           hub_count = 0;
#include "usb_hub_watch.h"
static HubStatusWatch hub_watch[MAX_HUBS] = {};

static bool hub_ack_hub_changes(USBDevice& hub) {
    ++g_usb_hub_status_requests;
    USBControlGuard guard;
    static volatile uint8_t* buffers[MAX_CONTROLLERS] = {};
    volatile uint8_t*& buf = buffers[hub.hc - xhci_hc];
    if (!buf) buf = (volatile uint8_t*)alloc_table();
    for (int b = 0; b < 4; ++b) buf[b] = 0;
    USBSetupPacket setup;
    setup.bmRequestType = 0xA0; setup.bRequest = 0;
    setup.wValue = 0; setup.wIndex = 0; setup.wLength = 4;
    if (!usb_control_ok(do_control_transfer(hub, &setup, buf, 4))) return false;
    uint8_t changes = buf[2];
    for (uint8_t bit = 0; bit < 2; ++bit) {
        if (!(changes & (1u << bit))) continue;
        setup.bmRequestType = 0x20; setup.bRequest = 1;
        setup.wValue = bit; setup.wLength = 0;
        if (!usb_control_ok(do_control_transfer(hub, &setup, nullptr, 0))) return false;
    }
    return true;
}

// USB 2 hub change bits are latched until CLEAR_FEATURE. A notification for
// reset completion/enable/suspend is not itself a reason to reset the bus.
static bool hub_ack_port_changes(USBDevice& hub, uint8_t port, const uint8_t* st) {
    uint16_t changes = (st[2] | ((uint16_t)st[3] << 8)) & usb_hub_change_mask(hub.speed);
    for (uint8_t bit = 0; bit < 8; ++bit)
        if ((changes & (1u << bit)) && !hub_clear_port_feature(hub, port, usb_hub_change_feature(hub.speed, bit))) return false;
    return true;
}

static void hub_prepare_watch(USBDevice* hub, uint8_t ports) {
    for (int h = 0; h < hub_count; ++h) {
        if (hubs[h] != hub) continue;
        HubStatusWatch& watch = hub_watch[h];
        watch.num_ports = ports;
        watch.report_bytes = (ports + 8u) / 8u;
        if (!hub_ack_hub_changes(*hub)) return;
        for (uint8_t p = 1; p <= ports; ++p) {
            uint8_t st[4];
            if (!hub_get_port_status(*hub, p, st) || !hub_ack_port_changes(*hub, p, st)) return;
            xhci_port_watch_init(watch.ports[p], st[0]);
        }
        watch.report = (volatile uint8_t*)alloc_table();
        watch.control.data = (volatile uint8_t*)alloc_table();
        watch.ready = true;
        watch.poll.cursor = 1;
        watch.poll.audit_after = _usb_rdtsc() + (_usb_tsc_hz() / 1000) * 250u;
        print((char*)"[usb] Hub status watch ready.");
    }
}

static void hub_queue_status(int h) {
    USBDevice* hub = hubs[h];
    HubStatusWatch& watch = hub_watch[h];
    if (!watch.ready || watch.pipe_failed || watch.armed || watch.scan_mask) return;
    for (uint8_t b = 0; b < watch.report_bytes; ++b) watch.report[b] = 0;
    ring_push_cmd(hub->hub_ring, (1u << 10) | (1u << 5),
                  (uint64_t)watch.report - HHDM, watch.report_bytes);
    __asm__ volatile("sfence" ::: "memory");
    watch.armed = true;
    hub->hc->doorbell32[hub->slot_id] = hub_dcis[h];
}

static void hub_note_report(int h) {
    HubStatusWatch& watch = hub_watch[h];
    uint32_t mask = 0;
    for (uint8_t b = 0; b < watch.report_bytes; ++b) mask |= (uint32_t)watch.report[b] << (b * 8u);
    mask &= (1u << (watch.num_ports + 1)) - 1u;
    watch.poll.pending |= mask;
    if (watch.scan_mask) watch.recheck_mask |= mask;
    watch.poll.pending_after = 0;
}

#include "usb_hub_async.h"


/* ── forward declaration ── */
static void enumerate_hub_children(USBDevice* hub, uint8_t max_depth, uint8_t only_port = 0);

/* ── enumerate_device_after_address ──────────────────────── */
static void enumerate_device_after_address(
		USBDevice*           dev,
		volatile uint8_t*    rt_base,
		uint32_t             /*hcs1*/,
		uint32_t             /*hcs2*/) {

	if (usb_probe_expired(dev->hc)) return;
	print((char*)"Enumerating device.");
	char str[64];
    
    uint32_t hcc1 = dev->hc->hcc1;

	size_t ctx_stride = (hcc1 & (1u << 2)) ? 0x40u : 0x20u;

	volatile uint8_t* scratch = (volatile uint8_t*)alloc_table();

	/* ── Step 1: read first 8 bytes to learn EP0 max packet size ── */
	if (!get_device_descriptor(*dev, scratch, 8)) {
		print((char*)"Failed to read device descriptor (8B)");
		return;
	}
	uint16_t mps0 = usb_ep0_packet_size(dev->speed, scratch[7]);
	if (!mps0) {
		print((char*)"Invalid EP0 packet size in device descriptor");
		return;
	}
	to_str(mps0, str); print((char*)"EP0 MPS:"); print(str);

	/* ── Step 2: Evaluate Context to update EP0 MPS ── */
	{
		volatile uint64_t* dev_ctx   = dev->device_ctx;

		volatile uint32_t* out_slot  = (volatile uint32_t*)((uint64_t)dev_ctx);
		volatile uint32_t* out_ep0   = (volatile uint32_t*)((uint64_t)dev_ctx + ctx_stride);

		volatile uint64_t* ictx      = alloc_table();
		uint64_t           ictx_phys = (uint64_t)ictx - HHDM;
		for (int j = 0; j < 512; j++) ictx[j] = 0;

		volatile InputControlCtx* icc = (volatile InputControlCtx*)ictx;
		icc->drop_flags = 0;
		icc->add_flags  = (1u << 0) | (1u << 1);

		volatile uint32_t* in_slot = (volatile uint32_t*)((uint64_t)ictx + ctx_stride);
		volatile uint32_t* in_ep0  = (volatile uint32_t*)((uint64_t)ictx + ctx_stride * 2);

		int dws = (int)(ctx_stride / 4);
		for (int d = 0; d < dws; d++) {
			in_slot[d] = out_slot[d];
			in_ep0[d]  = out_ep0[d];
		}

		in_ep0[1] = (in_ep0[1] & 0x0000FFFFu) | ((uint32_t)mps0 << 16);

		ring_push_cmd(&dev->hc->cr, (13u << 10) | (dev->slot_id << 24), ictx_phys, 0);
		dev->hc->doorbell32[0] = 0;

		USB_Response resp;
		uint64_t hz_eval = _usb_tsc_hz();
		uint64_t start_eval = _usb_rdtsc();
		uint64_t end_eval = usb_timeout_deadline(dev->hc, 1000);
		
		for (;;) {
			resp = get_usb_command_response(dev->hc, 10); // 10ms chunk
			if (resp.gotresponse && resp.type == 33) break;
			if (_usb_rdtsc() >= end_eval) {
				print((char*)"Evaluate Context timeout!");
				return;
			}
		}
		uint8_t code = (resp.event->status >> 24) & 0xFF;
		if (code != 1) {
			print((char*)"Evaluate Context failed, code:");
			to_str(code, str); print(str);
			return;
		}
	}

	/* ── Step 3: full device descriptor ── */
	if (!get_device_descriptor(*dev, scratch, 18)) {
		print((char*)"Failed to read device descriptor (18B)");
		return;
	}
	uint8_t dev_class = scratch[4];
	dev->vendor_id = (uint16_t)scratch[8] | ((uint16_t)scratch[9] << 8);
	dev->product_id = (uint16_t)scratch[10] | ((uint16_t)scratch[11] << 8);

	/* ── Step 4: configuration descriptor ── */
	volatile uint8_t* config_buf = (volatile uint8_t*)alloc_table();
	for (int i = 0; i < 512; i++) config_buf[i] = 0;
	if (!get_configuration_descriptor(*dev, config_buf, 512)) {
		print((char*)"Failed to read config descriptor");
		return;
	}

	/* ── Step 5: identify hub / HID interfaces ── */
	bool found_hub      = (dev_class == 0x09);
	uint8_t cfg_val     = config_buf[5];

	int  idx      = 0;
	int  conf_len = (int)((uint16_t)config_buf[2] | ((uint16_t)config_buf[3] << 8));
	if (conf_len < 9 || conf_len > 512) return;
	bool mass_interface = false;
	bool mass_found = false;
	uint8_t mass_in = 0, mass_out = 0;
	uint16_t mass_in_mps = 0, mass_out_mps = 0;
	uint8_t mass_in_burst = 0, mass_out_burst = 0;
	
	dev->interface_count = 0;
	bool current_iface_is_hid = false;
	uint8_t current_iface_num = 0;
	uint8_t current_iface_protocol = 0;
	uint8_t current_iface_subclass = 0;
	uint16_t current_report_length = 0;

	while (idx + 2 < conf_len) {
		uint8_t blen  = config_buf[idx];
		uint8_t btype = config_buf[idx + 1];
		if (blen < 2 || idx + blen > conf_len) return;
		
		if (btype == 4 && idx + 9 <= conf_len) { // Interface Descriptor
			mass_interface = !mass_found && config_buf[idx + 3] == 0 &&
			    config_buf[idx + 5] == 8 && config_buf[idx + 6] == 6 && config_buf[idx + 7] == 0x50;
			if (mass_interface) { mass_found = true; mass_in = mass_out = 0; }
			current_iface_is_hid = (config_buf[idx + 5] == 0x03);
			current_iface_num = config_buf[idx + 2];
			current_iface_protocol = config_buf[idx + 7];
			current_iface_subclass = config_buf[idx + 6];
			current_report_length = 0;
			if (config_buf[idx + 5] == 0x09) found_hub = true;
		} 
		else if (btype == 0x21 && current_iface_is_hid && blen >= 6) {
			for (uint8_t d = 0; d < config_buf[idx + 5]; ++d) {
				uint16_t entry = 6u + (uint16_t)d * 3u;
				if (entry + 3 > blen) return;
				if (config_buf[idx + entry] == 0x22)
					current_report_length = config_buf[idx + entry + 1] | ((uint16_t)config_buf[idx + entry + 2] << 8);
			}
		}
		else if (btype == 5 && idx + 7 <= conf_len) { // Endpoint Descriptor
			uint8_t addr = config_buf[idx + 2];
			uint8_t attr = config_buf[idx + 3];
			uint16_t mps = (uint16_t)config_buf[idx + 4] | ((uint16_t)config_buf[idx + 5] << 8);
			if (mass_interface && (attr & 3) == 2 && (addr & 15) && mps <= 1024) {
				USBSSCompanion companion{};
				if (dev->speed >= 4 && !usb_ss_companion(config_buf, conf_len, idx, companion)) {
					print((char*)"[storage] Invalid or unsupported SuperSpeed bulk companion descriptor."); return;
				}
				if (addr & 0x80) { mass_in = addr & 15; mass_in_mps = mps; mass_in_burst = companion.burst; }
				else { mass_out = addr & 15; mass_out_mps = mps; mass_out_burst = companion.burst; }
			}
			
			if (current_iface_is_hid && (addr & 0x80u) && (attr & 0x03u) == 0x03) { // Interrupt IN
				if (dev->interface_count < 4) {
					HIDInterface* hi = &dev->interfaces[dev->interface_count++];
					hi->ep_num = addr & 0x0F;
					hi->mps = mps;
					hi->protocol = current_iface_protocol;
					hi->iface_num = current_iface_num;
					hi->interval = config_buf[idx + 6];
					hi->report_descriptor_length = current_report_length;
					hi->boot_capable = current_iface_subclass == 1 && (hi->protocol == 1 || hi->protocol == 2);
					hi->ring = nullptr;
					
					// Type mapping: Protocol 1 = Keyboard, anything else HID = Mouse fallback
					if (current_iface_protocol == 1) hi->type = 1;
					else hi->type = 2;

					char dstr[64];
					print((char*)"[USB] HID Inf found. Type:");
					to_str(hi->type, dstr); print(dstr);
					print((char*)" Prot:"); to_str(hi->protocol, dstr); print(dstr);
					print((char*)" MPS:"); to_str(hi->mps, dstr); print(dstr);
				}
			}
		}
		idx += blen;
	}

	dev->is_hub = found_hub;

	if (dev->interface_count == 0 && !found_hub && !(mass_in && mass_out)) {
		print((char*)"Device has no HID interfaces nor hub; disabling slot.");

		// Issue Disable Slot so the xHC stops all activity on this slot and
		// frees its resources.  Without this, the device (e.g. a USB boot
		// drive) is left in the Addressed state with EP0 live.  The xHC can
		// then generate stray Transfer Events (NAKs, STALLs, errors on EP0)
		// that fill the event ring and cause keyboard events to be lost or
		// delayed in the main loop.
		ring_push_cmd(&dev->hc->cr, (10u << 10) | (dev->slot_id << 24), 0, 0);
		dev->hc->doorbell32[0] = 0;
		{
			USB_Response resp;
			uint64_t hz_ds = _usb_tsc_hz();
			uint64_t start_ds = _usb_rdtsc();
			uint64_t end_ds = start_ds + (hz_ds / 1000) * 500u; // 500ms
			while (_usb_rdtsc() < end_ds) {
				resp = get_usb_command_response(dev->hc, 10);
				if (resp.gotresponse && resp.type == 33) break;
			}
		}
		// Clear DCBAA entry so the slot is fully released.
		dev->hc->dcbaa[dev->slot_id] = 0;
		print((char*)"Slot disabled for non-supported device.");
		return;
	}

	/* ── SET_CONFIGURATION ── */
	{
		USBSetupPacket sc = {};
		sc.bmRequestType = 0x00;
		sc.bRequest      = 0x09;
		sc.wValue        = cfg_val;
		sc.wIndex        = 0;
		sc.wLength       = 0;
		volatile uint8_t* dummy = (volatile uint8_t*)alloc_table();
		USB_Response r = do_control_transfer(*dev, &sc, dummy, 0);
		if (!usb_control_ok(r)) {
			print((char*)"SET_CONFIGURATION failed");
			return;
		} else {
			print((char*)"SET_CONFIGURATION ok");
		}
	}

	if (mass_in && mass_out) {
		USBProbePause media_readiness(dev->hc);
		if (!usb_storage_configure(dev, mass_in, mass_in_mps, mass_out, mass_out_mps, mass_in_burst, mass_out_burst))
			print((char*)"[storage] Unsupported or unavailable USB disk.");
		return;
	}

	/* ════════════════════════════════════════════════════════════
	   Hub path — configure the hub's status-change interrupt IN
	   endpoint and register the hub so kmain can queue and service
	   it exactly like a keyboard's interrupt IN endpoint.

	   This mirrors what Linux does in hub_probe() / hub_activate():
	   find the status-change endpoint, submit a persistent interrupt
	   URB (hub_irq()), and re-submit after every completion.  When
	   downstream ports change state, the hub sends a bitmap. Read and
	   acknowledge the latched status before rearming the endpoint.
	   ════════════════════════════════════════════════════════════ */
	if (found_hub) {
		print((char*)"Found hub; setting up status-change interrupt pipe.");

		// ── Find interrupt IN endpoint in config descriptor ──────
		uint8_t  hub_ep_num  = 0;
		uint16_t hub_ep_mps  = 1;
		uint8_t  hub_ep_ivl  = 0xFF;
		USBSSCompanion hub_companion{};

		int scan     = 0;
		int scan_len = (int)((uint16_t)config_buf[2] | ((uint16_t)config_buf[3] << 8));
		while (scan + 2 < scan_len) {
			uint8_t blen  = config_buf[scan];
			uint8_t btype = config_buf[scan + 1];
			if (blen == 0) break;
			if (btype == 5 && scan + 7 <= scan_len) {
				uint8_t  addr = config_buf[scan + 2];
				uint8_t  attr = config_buf[scan + 3];
				uint16_t mps  = (uint16_t)config_buf[scan + 4]
				              | ((uint16_t)config_buf[scan + 5] << 8);
				uint8_t  ivl  = config_buf[scan + 6];
				// Interrupt IN: direction bit set, transfer type == 3
				if ((addr & 0x80u) && (attr & 0x03u) == 3) {
					if (dev->speed >= 4 && !usb_ss_companion(config_buf, scan_len, scan, hub_companion)) {
						print((char*)"[usb] Invalid SuperSpeed hub interrupt companion descriptor."); return;
					}
					hub_ep_num = addr & 0x0Fu;
					hub_ep_mps = mps;
					hub_ep_ivl = ivl;
					break;
				}
			}
			scan += blen;
		}

		if (hub_ep_num == 0) {
			print((char*)"Hub has no interrupt IN endpoint; hub reconnects won't fire events.");
			// Children can still be enumerated; we just can't detect
			// downstream reconnects without the status-change pipe.
			return;
		}

		// DCI for an IN endpoint: ep_num * 2 + 1  (xHCI spec §4.8.1)
		uint8_t hub_dci = (uint8_t)(hub_ep_num * 2u + 1u);
		to_str(hub_dci, str); print((char*)"Hub interrupt IN DCI:"); print(str);

		// ── Configure the endpoint via Configure Endpoint command ──
		struct Ring* hub_ring = (struct Ring*)alloc_table();
		ring_init(hub_ring);
		dev->hub_ring = hub_ring;

		volatile uint64_t* ictx2      = alloc_table();
		uint64_t           ictx2_phys = (uint64_t)ictx2 - HHDM;
		for (int j = 0; j < 512; j++) ictx2[j] = 0;

		volatile InputControlCtx* icc2 = (volatile InputControlCtx*)ictx2;
		icc2->drop_flags = 0;
		icc2->add_flags  = (1u << 0) | (1u << hub_dci);

		volatile uint32_t* dev_slot_out = (volatile uint32_t*)((uint64_t)dev->device_ctx);
		volatile uint32_t* dev_ep0_out  = (volatile uint32_t*)((uint64_t)dev->device_ctx + ctx_stride);

		volatile uint32_t* in_slot2   = (volatile uint32_t*)((uint64_t)ictx2 + ctx_stride);
		volatile uint32_t* in_ep02    = (volatile uint32_t*)((uint64_t)ictx2 + ctx_stride * 2);
		// Context slot for hub_dci lives at stride * (hub_dci + 1) because
		// the input context array is: [ctrl][slot][ep0][ep1out][ep1in]...
		volatile uint32_t* in_hub_ep  = (volatile uint32_t*)((uint64_t)ictx2 + ctx_stride * ((uint64_t)hub_dci + 1u));

		int dws2 = (int)(ctx_stride / 4);
		for (int j = 0; j < dws2; j++) {
			in_slot2[j]  = dev_slot_out[j];
			in_ep02[j]   = dev_ep0_out[j];
			in_hub_ep[j] = 0;
		}

		// Update Context Entries in slot DW0[31:27] to cover hub_dci.
		in_slot2[0] = (in_slot2[0] & ~(0x1Fu << 27)) | ((uint32_t)hub_dci << 27);

		uint8_t interval = xhci_interrupt_interval(dev->speed, hub_ep_ivl);
		in_hub_ep[0] = ((uint32_t)interval << 16);
		// EP type 7 = Interrupt IN, CErr = 3
		in_hub_ep[1] = (3u << 1) | (7u << 3) | ((uint32_t)hub_companion.burst << 8) | ((uint32_t)hub_ep_mps << 16);
		uint64_t deq = hub_ring->phys | 1u;
		in_hub_ep[2] = (uint32_t)(deq & 0xFFFFFFFFu);
		in_hub_ep[3] = (uint32_t)(deq >> 32);
		uint16_t interval_bytes = dev->speed >= 4 ? hub_companion.interval_bytes : hub_ep_mps;
		in_hub_ep[4] = (uint32_t)hub_ep_mps | ((uint32_t)interval_bytes << 16);

		ring_push_cmd(&dev->hc->cr, (12u << 10) | (dev->slot_id << 24), ictx2_phys, 0);
		dev->hc->doorbell32[0] = 0;

		{
			USB_Response resp;
			uint64_t hz_hub = _usb_tsc_hz();
			uint64_t start_hub = _usb_rdtsc();
			uint64_t end_hub = usb_timeout_deadline(dev->hc, 1000);
			for (;;) {
				resp = get_usb_command_response(dev->hc, 10);
				if (resp.gotresponse && resp.type == 33) break;
				if (_usb_rdtsc() >= end_hub) break;
			}
		uint32_t code = resp.gotresponse ? (resp.event->status >> 24) & 0xFF : 0;
			if (code != 1) {
				print((char*)"Hub interrupt EP configure failed, code:");
				to_str(code, str); print(str);
				// Not fatal for the hub's children — enumeration can still
				// proceed; we just cannot receive status-change notifications.
				return;
			}
			print((char*)"Hub interrupt IN endpoint configured.");
		}

		// ── Register hub so kmain queues a TRB and services events ──
		// (same pattern as keyboards[] / keyboard_count)
		if (hub_count < MAX_HUBS) {
			for (size_t b = 0; b < sizeof(HubStatusWatch); ++b)
				((volatile uint8_t*)&hub_watch[hub_count])[b] = 0;
			hubs[hub_count]     = dev;
			hub_dcis[hub_count] = hub_dci;
			hub_count++;
			print((char*)"Hub registered for status-change monitoring.");
		} else {
			print((char*)"Hub array full; status-change pipe not monitored.");
		}

		// Do NOT queue the initial TRB here — same reason as keyboards:
		// any resulting Transfer Event would be discarded by the command-
		// completion polling loops still running inside enumeration.
		// kmain queues one TRB per hub after all enumeration is complete.
		return;
	}

	/* ── HID-specific setup (Keyboard & Mouse) ── */



	for (int i = 0; i < dev->interface_count; i++) {
		HIDInterface* hi = &dev->interfaces[i];
		if (usb_is_g502_hero(*dev)) {
			usb_g502_prepare(*dev, *hi);
			continue;
		}
		if (hi->type == 2 && usb_hid_pointer_prepare(*dev, *hi)) continue;
		USBSetupPacket bp = {};
		bp.bmRequestType = 0x21;
		bp.bRequest      = 0x0B;
		bp.wValue        = 0; // Boot Protocol (if supported)
		bp.wIndex        = hi->iface_num;
		bp.wLength       = 0;
		volatile uint8_t* tmp = (volatile uint8_t*)alloc_table();
		do_control_transfer(*dev, &bp, tmp, 0);
		
		USBSetupPacket si = {};
		si.bmRequestType = 0x21;
		si.bRequest      = 0x0A;
		si.wValue        = 0;
		si.wIndex        = hi->iface_num;
		si.wLength       = 0;
		do_control_transfer(*dev, &si, tmp, 0);
	}

	/* ── Configure interrupt IN endpoints ── */

	volatile uint64_t* ictx2 = alloc_table();
	uint64_t ictx2_phys = (uint64_t)ictx2 - HHDM;
	for (int j = 0; j < 512; j++) ictx2[j] = 0;

	volatile InputControlCtx* icc2 = (volatile InputControlCtx*)ictx2;
	icc2->drop_flags = 0;
	icc2->add_flags  = (1u << 0);
	
	uint8_t max_dci = 1;
	for (int i = 0; i < dev->interface_count; i++) {
		HIDInterface* hi = &dev->interfaces[i];
		struct Ring* r = (struct Ring*)alloc_table();
		ring_init(r);
		hi->ring = r;
		
		uint8_t dci = hi->ep_num * 2 + 1;
		icc2->add_flags = icc2->add_flags | (1u << dci);
		if (dci > max_dci) max_dci = dci;
	}

	volatile uint32_t* dev_slot_out = (volatile uint32_t*)((uint64_t)dev->device_ctx);
	volatile uint32_t* dev_ep0_out  = (volatile uint32_t*)((uint64_t)dev->device_ctx + ctx_stride);

	volatile uint32_t* in_slot2 = (volatile uint32_t*)((uint64_t)ictx2 + ctx_stride);
	volatile uint32_t* in_ep02  = (volatile uint32_t*)((uint64_t)ictx2 + ctx_stride * 2);

	int dws = (int)(ctx_stride / 4);
	for (int j = 0; j < dws; j++) {
		in_slot2[j] = dev_slot_out[j];
		in_ep02[j]  = dev_ep0_out[j];
	}

	in_slot2[0] = (in_slot2[0] & ~(0x1Fu << 27)) | ((uint32_t)max_dci << 27);

	for (int i = 0; i < dev->interface_count; i++) {
		HIDInterface* hi = &dev->interfaces[i];
		uint8_t interval = xhci_interrupt_interval(dev->speed, hi->interval);
		uint8_t dci = hi->ep_num * 2 + 1;
		volatile uint32_t* in_hi_ep = (volatile uint32_t*)((uint64_t)ictx2 + ctx_stride * ((uint64_t)dci + 1u));
		for (int j = 0; j < dws; j++) in_hi_ep[j] = 0;
		in_hi_ep[0] = ((uint32_t)interval << 16);
		in_hi_ep[1] = (3u << 1) | (7u << 3) | ((uint32_t)hi->mps << 16);
		uint64_t deq = hi->ring->phys | 1u;
		in_hi_ep[2] = (uint32_t)(deq & 0xFFFFFFFFu);
		in_hi_ep[3] = (uint32_t)(deq >> 32);
		in_hi_ep[4] = (uint32_t)hi->mps | ((uint32_t)hi->mps << 16);
	}

	ring_push_cmd(&dev->hc->cr, (12u << 10) | (dev->slot_id << 24), ictx2_phys, 0);
	dev->hc->doorbell32[0] = 0;

	{
		USB_Response resp;
		uint64_t hz_conf = _usb_tsc_hz();
		uint64_t start_conf = _usb_rdtsc();
		uint64_t end_conf = usb_timeout_deadline(dev->hc, 1000);
		for (;;) {
			resp = get_usb_command_response(dev->hc, 10);
			if (resp.gotresponse && resp.type == 33) break;
			if (_usb_rdtsc() >= end_conf) {
				resp.gotresponse = false;
				break;
			}
		}
		uint32_t code = 0;
		if (resp.gotresponse) code = (resp.event->status >> 24) & 0xFF;
		if (code != 1) {
			print((char*)"Configure Endpoint failed, code:");
			char str[64];
			to_str(code, str); print(str);
			return;
		}
		print((char*)"Configured IN endpoints for HID interface(s).");
	}

	bool has_keyboard = false, has_mouse = false;
	for (int i = 0; i < dev->interface_count; i++) {
		HIDInterface* hi = &dev->interfaces[i];
		hi->configured = true;
		if (hi->type == 1) has_keyboard = true;
		else if (hi->type == 2) has_mouse = true;
	}
	if (has_keyboard && keyboard_count < MAX_KEYBOARDS) keyboards[keyboard_count++] = dev;
	if (has_mouse && mouse_count < MAX_MICE) mice[mouse_count++] = dev;
}


/* ── configure_hub_slot ───────────────────────────────────── */
static void configure_hub_slot(USBDevice* hub, uint8_t num_ports, uint8_t tt_think_time = 0) {
	char str[64];
	size_t ctx_stride = (hub->hc->hcc1 & (1u << 2)) ? 0x40u : 0x20u;

	volatile uint64_t* ictx      = alloc_table();
	uint64_t           ictx_phys = (uint64_t)ictx - HHDM;
	for (int j = 0; j < 512; j++) ictx[j] = 0;

	volatile InputControlCtx* icc = (volatile InputControlCtx*)ictx;
	icc->drop_flags = 0;
	icc->add_flags  = (1u << 0);   // Slot context only

	volatile uint32_t* out_slot = (volatile uint32_t*)((uint64_t)hub->device_ctx);
	volatile uint32_t* in_slot  = (volatile uint32_t*)((uint64_t)ictx + ctx_stride);
	int dws = (int)(ctx_stride / 4);
	for (int d = 0; d < dws; d++) in_slot[d] = out_slot[d];

	// Set Hub bit in DW0[26]
	in_slot[0] = in_slot[0] | (1u << 26);
	if (hub->speed >= 4) {
		in_slot[0] = in_slot[0] & ~(1u << 25); // MTT applies only to high-speed USB 2 hubs.
		in_slot[2] = in_slot[2] & ~0x3ffffu;   // No transaction translator on a SuperSpeed hub.
	} else if (hub->speed == 3) {
		// Hub descriptor wHubCharacteristics[6:5] encodes 8/16/24/32 FS-bit
		// TT think time. Zero is not a safe assumption for every physical dock.
		in_slot[2] = (in_slot[2] & ~(3u << 16)) | ((uint32_t)(tt_think_time & 3u) << 16);
	}

	// Number of Ports lives in DW1[31:24], NOT DW1[15:8].
	// DW1[15:8] is the low byte of Max Exit Latency — corrupting that field
	// causes the hub slot to be mis-configured, making every child Address Device
	// fail with code 17 because the controller has no valid hub state to route
	// transactions through.
	in_slot[1] = (in_slot[1] & ~(0xFFu << 24)) | ((uint32_t)num_ports << 24);

	ring_push_cmd(&hub->hc->cr, (12u << 10) | (hub->slot_id << 24), ictx_phys, 0);
	hub->hc->doorbell32[0] = 0;

	USB_Response resp;
	uint64_t command_deadline = usb_timeout_deadline(hub->hc, 1000);
	for (;;) {
		resp = get_usb_command_response(hub->hc, 10);
		if (resp.gotresponse && resp.type == 33) break;
		if (_usb_rdtsc() >= command_deadline) {
			print((char*)"configure_hub_slot timed out");
			hub->hc->needsResetting = true;
			return;
		}
	}
	uint32_t code = (resp.event->status >> 24) & 0xFF;
	if (code != 1) {
		print((char*)"configure_hub_slot failed, code:");
		to_str(code, str); print(str);
		// Cannot enumerate children if the hub slot is not properly configured.
		hub->hc->needsResetting = true;
	} else {
		print((char*)"Hub slot configured.");
	}
}

/* ── enumerate_hub_children ───────────────────────────────── */
static void enumerate_hub_children(
		USBDevice*           hub,
		uint8_t              max_depth, uint8_t only_port) {

	if (max_depth == 0 || usb_probe_expired(hub->hc)) return;
	USBProbeGuard probe(hub->hc);
	if (hub->speed > 7) {
		print((char*)"[usb] Unsupported USB hub controller speed ID.");
		return;
	}
	char str[64];

	print((char*)"enumerate_hub_children entered");
	print((char*)"hub slot_id:"); to_str(hub->slot_id, str); print(str);

	uint8_t num_ports = 0;
    if (only_port) {
        for (int h = 0; h < hub_count; ++h) if (hubs[h] == hub) num_ports = hub_watch[h].num_ports;
        if (!num_ports || only_port > num_ports) return;
    } else {
	/* ── Get hub descriptor (inlined) ── */
	uint8_t hub_desc[16]; for (int i = 0; i < 16; i++) hub_desc[i] = 0;
	{
		volatile uint8_t* buf = (volatile uint8_t*)alloc_table();
		for (int i = 0; i < 4096; i++) buf[i] = 0;

		USBSetupPacket setup;
		setup.bmRequestType = 0xA0;
		setup.bRequest      = 0x06;
		setup.wValue        = (uint16_t)usb_hub_descriptor_type(hub->speed) << 8;
		setup.wIndex        = 0;
		setup.wLength       = hub->speed >= 4 ? 12 : 16;

		print((char*)"hub desc: sending control transfer");
		USB_Response r = do_control_transfer(*hub, &setup, buf, setup.wLength);
		print((char*)"hub desc: gotresponse="); to_str(r.gotresponse, str); print(str);

		if (!r.gotresponse) {
			print((char*)"FAIL: hub descriptor no response");
			return;
		}
		// Brief recovery before processing response
		tsc_delay_ms(2);

		uint32_t code = (r.event->status >> 24) & 0xFF;
		print((char*)"hub desc: code="); to_str(code, str); print(str);
		print((char*)"hub desc buf[0..7]:");
		for (int i = 0; i < 8; i++) { to_hex(buf[i], str); print(str); }

		if (code != 1 && code != 13) {
			print((char*)"FAIL: hub descriptor transfer failed");
			return;
		}

		for (int i = 0; i < 16; i++) hub_desc[i] = buf[i];
	}

	num_ports = hub_desc[2];
	print((char*)"Hub num_ports:"); to_str(num_ports, str); print(str);

	if (!usb_hub_descriptor_valid(hub->speed, hub_desc, sizeof(hub_desc))) {
		print((char*)"FAIL: implausible num_ports");
		for (int i = 0; i < 8; i++) { to_hex(hub_desc[i], str); print(str); }
		return;
	}

	// Tell the xHCI this slot is a hub before addressing any child.
	configure_hub_slot(hub, num_ports, (hub_desc[3] >> 5) & 3u);
	if (hub->hc->needsResetting) return;
	if (hub->speed >= 4) {
		int depth = usb_hub_route_depth(((volatile uint32_t*)hub->device_ctx)[0] & 0xfffffu);
		if (depth < 0 || depth > 4) { print((char*)"[usb] Invalid SuperSpeed hub depth."); return; }
		USBSetupPacket depth_request{};
		depth_request.bmRequestType = 0x20; depth_request.bRequest = 12;
		depth_request.wValue = depth;
		if (!usb_control_ok(do_control_transfer(*hub, &depth_request, nullptr, 0))) {
			print((char*)"[usb] SET_HUB_DEPTH failed."); return;
		}
	}
	// Power all downstream ports together, then honor the descriptor's
	// power-good delay once. Per-port delays can miss USB-C hub link training.
	for (uint8_t p = 1; p <= num_ports; ++p) {
		if (usb_probe_expired(hub->hc)) return;
		if ((hub_desc[3] & 3u) < 2 && !hub_set_port_feature(*hub, p, 8)) {
			print((char*)"[usb] Hub port power request failed."); return;
		}
	}
	uint32_t power_delay = (uint32_t)hub_desc[5] * 2;
	if (power_delay < 100) power_delay = 100;
	tsc_delay_ms(power_delay);

    }

	size_t ctx_stride = (hub->hc->hcc1 & (1u << 2)) ? 0x40u : 0x20u;

	for (uint8_t p = 1; p <= num_ports; p++) {
		if (only_port && p != only_port) continue;
		if (usb_probe_expired(hub->hc)) return;
		if (hub->hc->global_port_index >= 4095) {
			print((char*)"[usb] Hub port bookkeeping limit reached.");
			return;
		}
		hub->hc->global_port_index++;
		if (hub->hc->portfailed[hub->hc->global_port_index]) {
			print((char*)"Skipping previously failed hub port");
			continue;
		}

		print((char*)"Checking hub port:"); to_str(p, str); print(str);

		// Power the port (feature 8 = PORT_POWER)
		if (only_port) hub_set_port_feature(*hub, p, 8);

		// Wait for connection to stabilize. Poll up to 50ms.
		bool connected = false;
		for (int attempt = 0; attempt < 25; attempt++) {
			tsc_delay_ms(2);
			uint8_t st[4]; for (int i = 0; i < 4; i++) st[i] = 0;
			if (!hub_get_port_status(*hub, p, st)) {
				print((char*)"FAIL: get_port_status (pre-reset) failed");
				hub->hc->portfailed[hub->hc->global_port_index] = true;
				break;
			}
			if (st[0] & 1u) {
				connected = true;
				print((char*)"Pre-reset wPortStatus:");
				for (int j = 0; j < 4; j++) { to_hex(st[j], str); print(str); }
				break;
			}
		}

		if (!connected) {
			print((char*)"Nothing on this hub port, skipping");
			continue;
		}

		// Clear stale C_PORT_CONNECTION change bit (feature 16)
		hub_clear_port_feature(*hub, p, 16);

		// Request port reset (feature 4 = PORT_RESET)
		print((char*)"Requesting hub port reset");
		if (!hub_set_port_feature(*hub, p, 4)) {
			print((char*)"FAIL: SET_FEATURE PORT_RESET");
			hub->hc->portfailed[hub->hc->global_port_index] = true;
			continue;
		}

		// Poll for C_PORT_RESET (wPortChange bit 4) for up to 100ms.
		bool reset_done = false;
		for (int attempt = 0; attempt < 100; attempt++) {
			tsc_delay_ms(1);   // 1ms per poll, 100ms total budget
			uint8_t st[4]; for (int i = 0; i < 4; i++) st[i] = 0;
			if (!hub_get_port_status(*hub, p, st)) {
				print((char*)"FAIL: get_port_status during reset poll");
				hub->hc->portfailed[hub->hc->global_port_index] = true;
				break;
			}
			if (st[2] & (1u << 4)) {
				reset_done = true;
				print((char*)"Hub port reset complete, attempt:"); to_str(attempt, str); print(str);
				break;
			}
		}

		if (!reset_done) {
			print((char*)"FAIL: hub port reset timed out");
			hub->hc->portfailed[hub->hc->global_port_index] = true;
			continue;
		}

		// Clear C_PORT_RESET change bit (feature 20)
		hub_clear_port_feature(*hub, p, 20);

		// USB 2.0 §7.1.7.5: 10ms TRSTRCY recovery after reset before first
		// transaction. Hub-attached storage must receive the full recovery time.
		tsc_delay_ms(10);

		// Read final port status
		uint8_t status4[4]; for (int i = 0; i < 4; i++) status4[i] = 0;
		if (!hub_get_port_status(*hub, p, status4)) {
			print((char*)"FAIL: get_port_status (post-reset)");
			hub->hc->portfailed[hub->hc->global_port_index] = true;
			continue;
		}
		print((char*)"Post-reset wPortStatus:");
		for (int i = 0; i < 4; i++) { to_hex(status4[i], str); print(str); }

		if (!(status4[0] & 1u)) {
			print((char*)"PORT_CONNECTION=0 after reset, skipping");
			continue;
		}
		if (!(status4[0] & 2u)) {
			print((char*)"FAIL: PORT_ENABLE=0 after reset");
			hub->hc->portfailed[hub->hc->global_port_index] = true;
			continue;
		}

		// Hub port speed bits (USB 2.0 hub spec, wPortStatus):
		//   bit 9  = PORT_LOW_SPEED  → xHCI speed code 2
		//   bit 10 = PORT_HIGH_SPEED → xHCI speed code 3
		//   neither                  → Full Speed → xHCI speed code 1
		uint16_t port_status = (uint16_t)status4[0] | ((uint16_t)status4[1] << 8);
		uint32_t speed = usb_hub_child_speed(hub->speed, port_status);
		if (!speed) { print((char*)"[usb] Unsupported hub child speed/status."); continue; }
		print((char*)"Hub child speed:"); to_str(speed, str); print(str);

		// Preserve the whole hub chain and the upstream root port. Replacing
		// the route with just p works for one hub but misroutes nested children.
		volatile uint32_t* parent_slot = (volatile uint32_t*)hub->device_ctx;
		uint32_t route_string = parent_slot[0] & 0xfffffu;
		uint32_t route_shift = 0;
		while (route_shift < 20 && ((route_string >> route_shift) & 15u)) route_shift += 4;
		if (route_shift == 20) {
			print((char*)"[USB] Hub route depth exceeded; skipping child.");
			continue;
		}
		route_string |= (uint32_t)(p > 15 ? 15 : p) << route_shift;
		uint32_t root_port_context = parent_slot[1] & 0x00ff0000u;

		// Enable Slot
		print((char*)"Enabling slot for hub child");
		ring_push_cmd(&hub->hc->cr, (9u << 10), 0, 0);
		hub->hc->doorbell32[0] = 0;

		USB_Response resp = usb_wait_command(hub->hc, "Enable Slot for hub child");
		if (!resp.gotresponse) return;
		uint32_t code    = (resp.event->status >> 24) & 0xFF;
		uint32_t slot_id = (resp.ctrl >> 24) & 0xFF;
		if (code != 1) {
			print((char*)"FAIL: Enable Slot for hub child, code:"); to_str(code, str); print(str);
			hub->hc->portfailed[hub->hc->global_port_index] = true;
			continue;
		}
		print((char*)"Hub child slot:"); to_str(slot_id, str); print(str);

		volatile uint64_t* dev_ctx      = (volatile uint64_t*)alloc_table();
		uint64_t           dev_ctx_phys = (uint64_t)dev_ctx - HHDM;
		hub->hc->dcbaa[slot_id] = dev_ctx_phys;

		struct Ring* ep0 = (struct Ring*)alloc_table();
		ring_init(ep0);

		volatile uint64_t* ictx      = alloc_table();
		uint64_t           ictx_phys = (uint64_t)ictx - HHDM;
		for (int j = 0; j < 512; j++) ictx[j] = 0;

		volatile InputControlCtx* icc = (volatile InputControlCtx*)ictx;
		icc->drop_flags = 0;
		icc->add_flags  = (1u << 0) | (1u << 1);

		volatile uint32_t* slot_ctx = (volatile uint32_t*)((uint64_t)ictx + ctx_stride);
		volatile uint32_t* ep0_ctx  = (volatile uint32_t*)((uint64_t)ictx + ctx_stride * 2);

		slot_ctx[0] = ((speed & 0xFu) << 20) | (1u << 27) | route_string;
		slot_ctx[1] = root_port_context;
		slot_ctx[2] = 0;
		slot_ctx[3] = 0;

		if (hub->speed == 3 && speed < 3) {
			slot_ctx[2] = (hub->slot_id & 0xFFu)
			            | ((uint32_t)(p & 0xFFu) << 8);
		} else if (speed < 3) {
			// A slower intermediate hub keeps the transaction translator of
			// its upstream high-speed hub, including that ancestor's port.
			slot_ctx[2] = parent_slot[2] & 0xffffu;
		}

		uint16_t ep0_mps = (speed >= 4) ? 512u : (speed == 3 ? 64u : 8u);
		ep0_ctx[0] = 0;
		ep0_ctx[1] = (3u << 1) | (4u << 3) | ((uint32_t)ep0_mps << 16);
		uint64_t deq = ep0->phys | 1u;
		ep0_ctx[2] = (uint32_t)(deq & 0xFFFFFFFFu);
		ep0_ctx[3] = (uint32_t)(deq >> 32);
		ep0_ctx[4] = 8;

		print((char*)"Sending Address Device for hub child");
		for (uint8_t addr_attempts = 0; addr_attempts < 5; addr_attempts++) {
			if (usb_probe_expired(hub->hc)) return;
			if (addr_attempts > 0) {
				// Disable the bad slot
				ring_push_cmd(&hub->hc->cr, (10u << 10) | (slot_id << 24), 0, 0);
				hub->hc->doorbell32[0] = 0;
				resp = usb_wait_command(hub->hc, "Disable failed hub child slot");
				if (!resp.gotresponse) return;
				if ((resp.event->status >> 24) != 1) return;
				hub->hc->dcbaa[slot_id] = 0;
				// Exponential backoff: 100ms, 200ms, 300ms...
				// The old spin_delay(500000 * n) was uncalibrated and potentially
				// several seconds per attempt. 100ms per step is plenty.
				tsc_delay_ms(25 * addr_attempts);

				// Enable a fresh slot
				ring_push_cmd(&hub->hc->cr, (9u << 10), 0, 0);
				hub->hc->doorbell32[0] = 0;
				resp = usb_wait_command(hub->hc, "Re-enable hub child slot");
				if (!resp.gotresponse) return;
				uint32_t new_code = (resp.event->status >> 24) & 0xFF;
				if (new_code != 1) {
					print((char*)"Re-enable slot failed");
					hub->hc->portfailed[hub->hc->global_port_index] = true; goto next_port;
				}
				slot_id = (resp.ctrl >> 24) & 0xFF;
				hub->hc->dcbaa[slot_id] = dev_ctx_phys;
				print((char*)"New slot:"); to_str(slot_id, str); print(str);
			}

			ring_push_cmd(&hub->hc->cr, (11u << 10) | (slot_id << 24), ictx_phys, 0);
			hub->hc->doorbell32[0] = 0;
			resp = usb_wait_command(hub->hc, "Address Device for hub child");
			if (!resp.gotresponse) return;
			code = (resp.event->status >> 24) & 0xFF;
			if (code == 1) {
				print((char*)"Hub child addressed successfully");
				goto addr_done;
			}
			print((char*)"Address Device failed, code:"); to_str(code, str); print(str);
		}
		hub->hc->portfailed[hub->hc->global_port_index] = true;
		next_port: continue;
		addr_done:

		// USB spec §9.2.6.3: 2ms minimum recovery after SET_ADDRESS.
		// We use 3ms for fast enumeration.
		tsc_delay_ms(3);

		USBDevice* child     = (USBDevice*)alloc_table();
		child->slot_id       = (uint8_t)slot_id;
		child->port_num      = p;
		child->root_port_num = hub->root_port_num;
		usb_slot_devices[hub->hc - xhci_hc][slot_id] = child;
		// Address Device can replace the legacy SS input speed using an SSP
		// Sublink Speed notification. Preserve its negotiated output value for
		// subsequent EP0/endpoint configuration and nested hub enumeration.
		uint32_t negotiated_speed = (((volatile uint32_t*)dev_ctx)[0] >> 20) & 15u;
		child->speed         = negotiated_speed ? negotiated_speed : speed;
		child->ep0_ring      = ep0;
		child->ep0_ring_phys = ep0->phys;
		child->device_ctx    = dev_ctx;
		child->dev_ctx_phys  = dev_ctx_phys;
		child->is_hub        = false;
		child->interface_count = 0;
		child->hub_ring      = nullptr;
		child->hc            = hub->hc;
		child->parent_hub    = hub;

		enumerate_device_after_address(child, nullptr, 0, 0);
		if (hub->hc->needsResetting) {
			print((char*)"Hub child enum failed, cleaning up.");
			hub->hc->needsResetting = false;
			// Force is_hub/interface_count to 0 to trigger cleanup
			child->is_hub = false;
			child->interface_count = 0;
		}

		// Cleanup logic for failed hub children (Stop EP0, Disable Slot, Drain)
		if (!child->is_hub && child->interface_count == 0 && !usb_storage_is_device(child)) {
			if (usb_probe_expired(hub->hc)) return;
			print((char*)"Cleaning up non-functional hub child");

			// CRITICAL: Physically disable the hub port to kill the link.
			// This is the hub equivalent of clearing PED on a root port.
			// It stops the xHC from retrying the failed device and frees the bus.
			if (hub->speed < 4) hub_clear_port_feature(*hub, p, 1);
			else hub_set_port_feature(*hub, (uint16_t)p | (3u << 8), 5); // SS U3 link state.
			tsc_delay_ms(2);

			// Stop EP0
			ring_push_cmd(&hub->hc->cr, (15u << 10) | (child->slot_id << 24) | (1u << 16), 0, 0);
			hub->hc->doorbell32[0] = 0;
			uint64_t hz_se = _usb_tsc_hz();
			uint64_t end_se = _usb_rdtsc() + (hz_se / 1000) * 200u;
			while (_usb_rdtsc() < end_se) {
				resp = get_usb_command_response(hub->hc, 10);
				if (resp.gotresponse && resp.type == 33) break;
			}

			// Disable Slot
			ring_push_cmd(&hub->hc->cr, (10u << 10) | (child->slot_id << 24), 0, 0);
			hub->hc->doorbell32[0] = 0;
			uint64_t end_ds = _usb_rdtsc() + (hz_se / 1000) * 200u;
			while (_usb_rdtsc() < end_ds) {
				resp = get_usb_command_response(hub->hc, 10);
				if (resp.gotresponse && resp.type == 33) break;
			}
			
			// Drain

			hub->hc->dcbaa[child->slot_id] = 0;
			usb_slot_devices[hub->hc - xhci_hc][child->slot_id] = nullptr;
			hub->hc->portfailed[hub->hc->global_port_index] = true;
			continue;
		}

		if (child->is_hub) {
			enumerate_hub_children(child, max_depth - 1);
			if (hub->hc->needsResetting) {
				hub->hc->needsResetting = false;
				continue;
			}
		}
	}
	if (!only_port) hub_prepare_watch(hub, num_ports);
}

#include "usb_root_ports.h"

static bool usb_boot_has_storage() {
    for (int d = 0; d < usb_disk_count; ++d) if (usb_disks[d].disk.online) return true;
    return false;
}
static void usb_wait_for_boot_storage() {
    if (usb_boot_has_storage()) return;
    print((char*)"[storage] Waiting for USB system storage discovery.");
    uint64_t started = _usb_rdtsc(), ticks_per_ms = _usb_tsc_hz() / 1000;
    uint64_t deadline = started + ticks_per_ms * 1500u, next_hub_scan = started;
    // Startup only: roots remain fixed once userspace begins. Check the grace
    // deadline between probes; each actual enumeration has its existing bound.
    while (_usb_rdtsc() < deadline) {
        for (int c = 0; c < xhci_count; ++c) {
            XHCIController* hc = &xhci_hc[c];
            if (hc->unavailable || hc->needsResetting) continue;
            for (uint32_t p = 0; p < hc->max_ports; ++p) {
                usb_root_poll(hc, p, _usb_rdtsc());
                if (usb_boot_has_storage() || _usb_rdtsc() >= deadline) return;
            }
        }
        uint64_t now = _usb_rdtsc();
        if (now >= next_hub_scan) {
            next_hub_scan = now + ticks_per_ms * 50u;
            bool reconciled = false;
            for (int h = 0; h < hub_count && !reconciled; ++h) {
                USBDevice* hub = hubs[h];
                HubStatusWatch& watch = hub_watch[h];
                if (!watch.ready) continue;
                for (uint8_t p = 1; p <= watch.num_ports; ++p) {
                    // Already-present input/peripheral branches are untouched.
                    if (watch.ports[p].baseline) continue;
                    uint8_t status[4]{};
                    if (!hub_get_port_status(*hub, p, status)) {
                        // An errored EP0 must not be blindly reused. Retain its
                        // children/DMA; another root or hub can still supply disk.
                        watch.ready = false;
                        print((char*)"[usb] Boot hub status failed; existing devices retained.");
                        break;
                    }
                    if (xhci_port_watch_changed(watch.ports[p], status[0], _usb_rdtsc(), ticks_per_ms * 150u)) {
                        usb_hub_reconcile(hub, p);
                        reconciled = true; // Reconciliation can compact hub lists.
                        break;
                    }
                    if (_usb_rdtsc() >= deadline) return;
                }
            }
        }
        if (usb_boot_has_storage()) return;
        boot_splash_tick(); tsc_delay_ms(8);
    }
}

// Failure-only snapshot: distinguish discovery, transport and root-selection
// failures on hardware without enabling verbose success-path/timing logging.
static void usb_storage_failure_summary(void (*sink)(const char*) = nullptr) {
    char line[224]; char* cursor = line;
    auto emit = [&]() { if (sink) sink(line); else print(line); };
    auto text = [&](const char* s) {
        while (*s && cursor < line + sizeof(line) - 1) *cursor++ = *s++;
        *cursor = 0;
    };
    auto number = [&](uint64_t value) { char n[32]; to_str(value, n); text(n); };
    auto id = [&](USBDevice* dev) {
        char n[32]; to_hex(((uint32_t)dev->vendor_id << 16) | dev->product_id, n); text(n);
    };
    unsigned online = 0;
    for (int d = 0; d < usb_disk_count; ++d) if (usb_disks[d].disk.online) ++online;
    text("[storage] Discovery: controllers="); number(xhci_count);
    text(" hubs="); number(hub_count); text(" disks="); number(usb_disk_count);
    text(" online="); number(online); text(" keyboards="); number(keyboard_count); emit();
    for (int c = 0; c < xhci_count; ++c) {
        XHCIController* hc = &xhci_hc[c]; cursor = line;
        text("[storage] Controller: id="); number(c);
        text(" unavailable="); number(hc->unavailable); text(" reset="); number(hc->needsResetting);
        text(" ports="); number(hc->max_ports); emit();
        if (hc->unavailable || !hc->ops) continue;
        for (uint32_t p = 0; p < hc->max_ports; ++p) {
            uint32_t sc = *usb_root_portsc(hc, p);
            USBDevice* dev = usb_root_devices[c][p];
            if (!xhci_port_probe_needed(sc) && !dev && !usb_root_attempted[c][p]) continue;
            cursor = line; text("[storage] Root: controller="); number(c); text(" port="); number(p + 1);
            char value[32]; to_hex(sc, value); text(" PORTSC="); text(value);
            text(" attempted="); number(usb_root_attempted[c][p]); text(" slot="); number(dev ? dev->slot_id : 0);
            if (dev) { text(" speed="); number(dev->speed); text(" vid:pid="); id(dev); }
            emit();
        }
    }
    for (int h = 0; h < hub_count; ++h) {
        USBDevice* hub = hubs[h]; cursor = line;
        text("[storage] Hub: controller="); number(hub->hc - xhci_hc);
        text(" slot="); number(hub->slot_id); text(" parent="); number(hub->parent_hub ? hub->parent_hub->slot_id : 0);
        text(" port="); number(hub->port_num); text(" speed="); number(hub->speed);
        text(" vid:pid="); id(hub); text(" watch="); text(hub_watch[h].ready ? "ready" : "off"); emit();
    }
    for (int d = 0; d < usb_disk_count; ++d) {
        usb_storage* disk = &usb_disks[d]; cursor = line;
        text("[storage] Disk: slot="); number(disk->dev->slot_id);
        text(" speed="); number(disk->dev->speed); text(" vid:pid="); id(disk->dev);
        text(" online="); number(disk->disk.online); text(" sectors="); number(disk->disk.sectors); emit();
    }
}

/* ── setupUSB ─────────────────────────────────────────────── */
static void setupUSB(XHCIController* hc) {
	if (hc->unavailable) return;
	usb_storage_reset_controller(hc);
	for (int p = 0; p < 256; ++p) {
		usb_slot_devices[hc - xhci_hc][p] = nullptr;
		usb_root_devices[hc - xhci_hc][p] = nullptr;
		usb_root_attempted[hc - xhci_hc][p] = false;
		xhci_port_watch_init(usb_root_ready_watch[hc - xhci_hc][p], 0);
		usb_root_changes[hc - xhci_hc][p].replacement = false;
		usb_root_changes[hc - xhci_hc][p].last_change = 0;
	}
	uint64_t usb_virt_base = hc->virt_base;
	uint8_t usb_start = 0, usb_bus = hc->bus, usb_dev = hc->dev, usb_fn = hc->fn;
	
	hc->global_port_index = 0;
	hc->needsResetting    = false;

	// Reset keyboard list so a retry pass does not accumulate stale entries
	// from the failed first pass. We only clear devices belonging to THIS controller.
	int new_kbd = 0;
	for (int k = 0; k < keyboard_count; k++) {
		if (keyboards[k] && keyboards[k]->hc != hc) keyboards[new_kbd++] = keyboards[k];
	}
	keyboard_count = new_kbd;
	
	int new_mouse = 0;
	for (int m = 0; m < mouse_count; m++) {
		if (mice[m] && mice[m]->hc != hc) mice[new_mouse++] = mice[m];
	}
	mouse_count = new_mouse;

	int new_hub = 0;
	for (int h = 0; h < hub_count; h++) {
		if (hubs[h] && hubs[h]->hc != hc) {
			hubs[new_hub]     = hubs[h];
			hub_dcis[new_hub] = hub_dcis[h];
			if (new_hub != h) for (size_t b = 0; b < sizeof(HubStatusWatch); ++b)
				((volatile uint8_t*)&hub_watch[new_hub])[b] = ((volatile uint8_t*)&hub_watch[h])[b];
			new_hub++;
		}
	}
	hub_count = new_hub;

	char str[64];

	uint32_t vid_did = pci_cfg_read32(usb_virt_base, usb_start, usb_bus, usb_dev, usb_fn, 0x00);
	uint16_t vendor  = (uint16_t)(vid_did & 0xFFFF);

	uint16_t pcmd = pci_cfg_read16(usb_virt_base, usb_start, usb_bus, usb_dev, usb_fn, 0x04);
	pcmd |= (1u << 1) | (1u << 2);
	pci_cfg_write16(usb_virt_base, usb_start, usb_bus, usb_dev, usb_fn, 0x04, pcmd);

	if (vendor == 0x8086) {
		print((char*)"Applying Intel xHCI routing...");
		intel_route_all_ports(usb_virt_base, usb_start, usb_bus, usb_dev, usb_fn);
	}

	uint32_t bar0     = pci_cfg_read32(usb_virt_base, usb_start, usb_bus, usb_dev, usb_fn, 0x10);
	uint32_t bar_type = (bar0 >> 1) & 0x3;
	uint64_t bar_addr;

	if ((bar0 & 0x1) == 1) { print((char*)"IO space BAR"); hcf(); }

	if (bar_type == 0x2) {
		uint32_t bar1_high = pci_cfg_read32(usb_virt_base, usb_start, usb_bus, usb_dev, usb_fn, 0x14);
		bar_addr = ((uint64_t)bar1_high << 32) | (bar0 & ~0xFULL);
	} else {
		bar_addr = bar0 & ~0xFULL;
	}

	pci_cfg_write32(usb_virt_base, usb_start, usb_bus, usb_dev, usb_fn, 0x10, 0xFFFFFFFF);
	uint32_t size_mask = pci_cfg_read32(usb_virt_base, usb_start, usb_bus, usb_dev, usb_fn, 0x10);
	pci_cfg_write32(usb_virt_base, usb_start, usb_bus, usb_dev, usb_fn, 0x10, bar0);
	uint64_t mmio_size = ~(size_mask & ~0xFu) + 1u;

	to_hex(bar_addr, str);   print(str);
	to_str(mmio_size, str);  print(str);

	// Multi-controller: offset the mapping base so they don't overlap using pointer arithmetic offset
	int hc_index = (int)(hc - xhci_hc);
	uint64_t mmio_va = USB_VA_BASE + (uint64_t)hc_index * 0x100000ULL; // 1MB spacing

	map_mmio_region(bar_addr, mmio_va, mmio_size);
	print((char*)"Mapped USB MMIO");

	volatile uint32_t* caps = (volatile uint32_t*)mmio_va;
	uint32_t info    = caps[0];
	uint32_t caplen  = info & 0xFF;
	uint32_t ver     = (info >> 16) & 0xFFFF;
	uint32_t hcs1    = caps[1];
	uint32_t hcs2    = caps[2];
	uint32_t hcc1    = caps[4];
	uint32_t dboff   = caps[5];
	uint32_t rtsoff  = caps[6];

	hc->hcc1 = hcc1;

	bool has_ac64 = (hcc1 & 1u);
	if (!has_ac64) print((char*)"WARNING: xHCI controller is 32-bit only!");

	xhci_legacy_handoff(hcc1, mmio_va, mmio_size);

	print((char*)"xHCI version:");
	to_str(ver, str); print(str);

	volatile XHCIOpRegs* ops = (volatile XHCIOpRegs*)((uintptr_t)mmio_va + caplen);

	hc->ops = ops;

	hc->doorbell32             = (volatile uint32_t*)(mmio_va + (dboff & ~0x3u));
	volatile uint8_t* rt_base = (volatile uint8_t*)(mmio_va + (rtsoff & ~0x1Fu));

	volatile uint32_t* iman   = (volatile uint32_t*)(rt_base + 0x20 + 0x00);
	volatile uint32_t* erstsz = (volatile uint32_t*)(rt_base + 0x20 + 0x08);
	volatile uint64_t* erstba = (volatile uint64_t*)(rt_base + 0x20 + 0x10);
	hc->erdp                  = (volatile uint64_t*)(rt_base + 0x20 + 0x18);

	// Reset controller
	ops->usbcmd &= ~1u;
	if (!usb_wait_register(hc, &ops->usbsts, 1u, 1u, 1000, "halt xHCI")) return;
	ops->usbcmd |= (1u << 1);
	if (!usb_wait_register(hc, &ops->usbcmd, 2u, 0, 1000, "reset xHCI")) return;
	if (!usb_wait_register(hc, &ops->usbsts, 1u << 11, 0, 1000, "xHCI ready")) return;

	// Command ring
	ring_init(&hc->cr);
	ops->crcr = (hc->cr.phys & ~0x3FULL) | 1u;

	// Event ring
	hc->er_virt = (volatile TRB*)alloc_table();
	hc->er_phys = (uint64_t)hc->er_virt - HHDM;
	for (int i = 0; i < ER_RING_SIZE; i++) {
		hc->er_virt[i].parameter = 0;
		hc->er_virt[i].status    = 0;
		hc->er_virt[i].control   = 0;
	}

	volatile ERSTEntry* erst      = (volatile ERSTEntry*)alloc_table();
	uint64_t            erst_phys = (uint64_t)erst - HHDM;
	erst[0].ring_segment_base = hc->er_phys;
	erst[0].ring_segment_size = ER_RING_SIZE;
	erst[0].reserved          = 0;

	*erstsz = 1;
	*erstba = erst_phys;
	*hc->erdp = hc->er_phys;
	*iman  |= (1u << 1) | (1u << 0);

	hc->ccs        = 1;
	hc->erdp_index = 0;

	// DCBAA
	uint32_t max_slots = hcs1 & 0xFF;
	volatile uint64_t* dcbaa_virt = (volatile uint64_t*)alloc_table();
	uint64_t           dcbaa_phys = (uint64_t)dcbaa_virt - HHDM;
	for (uint32_t i = 0; i < max_slots + 1; i++) dcbaa_virt[i] = 0;

	// Scratchpad buffers
	uint32_t sp_lo    = (hcs2 & 0x1Fu);
	uint32_t sp_hi    = (hcs2 >> 27) & 0x1Fu;
	uint32_t sp_count = (sp_hi << 5) | sp_lo;
	if (sp_count) {
		volatile uint64_t* spa      = (volatile uint64_t*)alloc_table();
		uint64_t           spa_phys = (uint64_t)spa - HHDM;
		for (uint32_t i = 0; i < sp_count; i++) {
			volatile uint8_t* pg = (volatile uint8_t*)alloc_table();
			spa[i] = (uint64_t)pg - HHDM;
		}
		dcbaa_virt[0] = spa_phys;
	}

	hc->dcbaa = dcbaa_virt;

	ops->dcbaap = dcbaa_phys;
	ops->config = max_slots;

	ops->usbcmd |= 1u;
	if (!usb_wait_register(hc, &ops->usbsts, 1u, 0, 1000, "run xHCI")) return;
	ops->crcr = (hc->cr.phys & ~0x3FULL) | 1u;

	print((char*)"xHCI controller running.");

	uint8_t  max_ports  = (hcs1 >> 24) & 0xFF;
	hc->max_ports       = max_ports;
	
	if (!hc->portfailed) {
	    hc->portfailed = (volatile bool*)alloc_table();
	}
	
	bool     needs_ppc  = (hcc1 & (1u << 3));

	if (needs_ppc) print((char*)"Manual port power required.");

	constexpr uint32_t PORTSC_CCS    = 1u << 0;
	constexpr uint32_t PORTSC_PED    = 1u << 1;
	constexpr uint32_t PORTSC_PR     = 1u << 4;
	constexpr uint32_t PORTSC_PP     = 1u << 9;

	// Flush any PSC events queued during controller start-up — no fixed spin.
	// Linux doesn't wait here either; PORTSC is read directly during port scan.
	// 5ms gives firmware time to post initial events before we drain them.
	tsc_delay_ms(5);
	{
		USB_Response resp;
		for (uint32_t drained = 0; drained < ER_RING_SIZE * 2; ++drained) {
			resp = get_usb_response(hc, 1);
			if (!resp.gotresponse) break;
			if (resp.type == 34) {
				uint32_t port_id = (resp.event->parameter >> 24) & 0xFF;
				print((char*)"Early PSC on port:"); to_str(port_id, str); print(str);
			}
		}
	}

	print((char*)"Scanning root ports...");

	// Power all ports before waiting. Every port gets the same 100 ms link
	// settling allowance; empty ports must not each add another 100 ms to boot.
	for (uint32_t i = 0; i < max_ports; ++i) {
		volatile uint32_t* portsc = (volatile uint32_t*)((uintptr_t)hc->ops + 0x400 + i * 0x10);
		if (needs_ppc) *portsc = (*portsc & ~0x00FE0002u) | PORTSC_PP;
	}
	tsc_delay_ms(100);

	for (uint32_t i = 0; i < max_ports; ++i) {
        ++hc->global_port_index;
        if (xhci_port_probe_needed(*usb_root_portsc(hc, i))) usb_root_enumerate(hc, i, true);
    }

	print((char*)"Root port enumeration complete.");

	// Snapshot every port, including ports above 32. Acknowledge enumeration's
	// reset/link notifications without disabling ports or starting another reset.
	for (uint32_t i = 0; i < max_ports; i++) {
		volatile uint32_t* portsc = (volatile uint32_t*)((uintptr_t)hc->ops + 0x400 + i * 0x10);
		uint32_t sc = *portsc;
		xhci_port_watch_init(usb_root_watch[hc - xhci_hc][i], sc);
		if (sc & (127u << 17)) *portsc = xhci_port_ack(sc);
	}
}

#endif // usbhelp_h
