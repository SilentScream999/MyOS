// src/usbhelpers_fixed.h
#ifndef usbhelp_h
#define usbhelp_h

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "structures.h"
#include "pagingstuff.h"
#include "helpers.h"
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
static volatile uint32_t*  doorbell32 = nullptr;

static volatile uint64_t*  g_dcbaa = nullptr;
static uint32_t             g_hcc1  = 0;

// Exposed so kmain can read PORTSC registers to detect disconnects without
// relying solely on PSC events, which some controllers do not generate
// reliably on disconnect.
static volatile XHCIOpRegs* g_ops      = nullptr;
static uint8_t              g_max_ports = 0;

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
static void tsc_delay_ms(uint32_t ms) {
    static uint64_t hz = 0;
    if (!hz) hz = _usb_tsc_hz();
    uint64_t end = _usb_rdtsc() + hz * (uint64_t)ms / 1000ULL;
    while (_usb_rdtsc() < end) { __asm__ volatile ("pause"); }
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

/* ── Event ring state ─────────────────────────────────────── */
static volatile TRB*      er_virt    = nullptr;
static uint8_t            ccs        = 1;
static uint8_t            erdp_index = 0;
static volatile uint64_t* erdp       = nullptr;
static uint64_t           er_phys    = 0;

/* ── get_usb_response ─────────────────────────────────────── */
static USB_Response get_usb_response(int timeout = 1000000) {
	while (timeout-- > 0) {
		TRB* evt  = (TRB*)&er_virt[erdp_index];
		uint32_t ctrl = evt->control;

		if ((ctrl & 1u) != ccs) {
			for (volatile int i = 0; i < 10; i++);
			continue;
		}

		erdp_index = (erdp_index + 1) % ER_RING_SIZE;
		if (erdp_index == 0) ccs ^= 1;
		*erdp = (er_phys + (uint64_t)erdp_index * 16u) | (1ull << 3);

		USB_Response resp;
		resp.gotresponse = true;
		resp.event       = evt;
		resp.type        = (ctrl >> 10) & 0x3F;
		resp.ctrl        = ctrl;
		return resp;
	}

	USB_Response resp;
	resp.gotresponse = false;
	return resp;
}

/* ── do_control_transfer ──────────────────────────────────── */
static USB_Response do_control_transfer(USBDevice &dev,
										USBSetupPacket* setup,
										volatile uint8_t* data_buffer,
										uint16_t data_len) {
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
		uint64_t data_phys = (uint64_t)data_buffer - HHDM;
		ring_push_cmd(dev.ep0_ring,
			(TRB_TYPE_DATA_STAGE << 10) | (data_is_in ? (1u << 16) : 0u),
			data_phys, (uint32_t)data_len);
	}

	uint32_t status_dir = data_is_in ? 0u : (1u << 16);
	ring_push_cmd(dev.ep0_ring,
		(TRB_TYPE_STATUS_STAGE << 10) | status_dir | (1u << 5),
		0, 0);

	doorbell32[dev.slot_id] = 1;

	USB_Response resp;
	resp.gotresponse = false;
	for (int j = 0; j < 5000000; j++) {
		resp = get_usb_response(1);
		if (!resp.gotresponse) continue;
		if (resp.type == 32 || resp.type == 33) return resp;
	}

	resp.gotresponse = false;
	return resp;
}

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
	if (!r.gotresponse) return false;
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
	if (!r.gotresponse) { print((char*)"cfg: no response"); return false; }

	uint16_t total = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
	if (total == 0) { print((char*)"cfg: wTotalLength=0"); return false; }
	if (total > maxlen) total = maxlen;

	for (int i = 0; i < total; i++) outbuf[i] = buf[i];
	return true;
}

/* ── Hub class helpers ────────────────────────────────────── */
static bool hub_set_port_feature(USBDevice &hub, uint16_t port, uint16_t feature) {
	volatile uint8_t* tmp = (volatile uint8_t*)alloc_table();
	for (int i = 0; i < 4096; i++) tmp[i] = 0;

	USBSetupPacket setup;
	setup.bmRequestType = 0x23;
	setup.bRequest      = 0x03;   // SET_FEATURE
	setup.wValue        = feature;
	setup.wIndex        = port;
	setup.wLength       = 0;

	USB_Response r = do_control_transfer(hub, &setup, tmp, 0);
	return r.gotresponse;
}

static bool hub_clear_port_feature(USBDevice &hub, uint16_t port, uint16_t feature) {
	volatile uint8_t* tmp = (volatile uint8_t*)alloc_table();
	for (int i = 0; i < 4096; i++) tmp[i] = 0;

	USBSetupPacket setup;
	setup.bmRequestType = 0x23;
	setup.bRequest      = 0x01;   // CLEAR_FEATURE
	setup.wValue        = feature;
	setup.wIndex        = port;
	setup.wLength       = 0;

	USB_Response r = do_control_transfer(hub, &setup, tmp, 0);
	return r.gotresponse;
}

static bool hub_get_port_status(USBDevice &hub, uint16_t port, uint8_t* status4) {
	volatile uint8_t* tmp = (volatile uint8_t*)alloc_table();
	for (int i = 0; i < 4096; i++) tmp[i] = 0;

	USBSetupPacket setup;
	setup.bmRequestType = 0xA3;
	setup.bRequest      = 0x00;   // GET_STATUS
	setup.wValue        = 0;
	setup.wIndex        = port;
	setup.wLength       = 4;

	USB_Response r = do_control_transfer(hub, &setup, tmp, 4);
	if (!r.gotresponse) return false;
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

static  volatile bool* portfailed        = nullptr;
static  bool           needsResetting    = true;
static  int            global_port_index = 0;

/* ── forward declaration ── */
static void enumerate_hub_children(USBDevice* hub, volatile XHCIOpRegs* ops,
									struct Ring &cr, uint8_t max_depth);

/* ── enumerate_device_after_address ──────────────────────── */
static void enumerate_device_after_address(
		USBDevice*           dev,
		volatile XHCIOpRegs* ops,
		volatile uint8_t*    rt_base,
		struct Ring&         cr,
		uint32_t             hcc1,
		uint32_t             /*hcs1*/,
		uint32_t             /*hcs2*/) {

	print((char*)"Enumerating device.");
	char str[64];

	size_t ctx_stride = (hcc1 & (1u << 2)) ? 0x40u : 0x20u;

	volatile uint8_t* scratch = (volatile uint8_t*)alloc_table();

	/* ── Step 1: read first 8 bytes to learn EP0 max packet size ── */
	if (!get_device_descriptor(*dev, scratch, 8)) {
		print((char*)"Failed to read device descriptor (8B)");
		return;
	}
	uint8_t mps0 = scratch[7];
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

		ring_push_cmd(&cr, (13u << 10) | (dev->slot_id << 24), ictx_phys, 0);
		doorbell32[0] = 0;

		USB_Response resp;
		for (;;) {
			resp = get_usb_response();
			if (!resp.gotresponse) continue;
			if (resp.type == 33) break;
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
	
	dev->interface_count = 0;
	bool current_iface_is_hid = false;
	uint8_t current_iface_num = 0;
	uint8_t current_iface_protocol = 0;

	while (idx + 2 < conf_len) {
		uint8_t blen  = config_buf[idx];
		uint8_t btype = config_buf[idx + 1];
		if (blen == 0) break;
		
		if (btype == 4 && idx + 9 <= conf_len) { // Interface Descriptor
			current_iface_is_hid = (config_buf[idx + 5] == 0x03);
			current_iface_num = config_buf[idx + 2];
			current_iface_protocol = config_buf[idx + 7];
			if (config_buf[idx + 5] == 0x09) found_hub = true;
		} 
		else if (btype == 5 && idx + 7 <= conf_len) { // Endpoint Descriptor
			uint8_t addr = config_buf[idx + 2];
			uint8_t attr = config_buf[idx + 3];
			uint16_t mps = (uint16_t)config_buf[idx + 4] | ((uint16_t)config_buf[idx + 5] << 8);
			
			if (current_iface_is_hid && (addr & 0x80u) && (attr & 0x03u) == 0x03) { // Interrupt IN
				if (dev->interface_count < 4) {
					HIDInterface* hi = &dev->interfaces[dev->interface_count++];
					hi->ep_num = addr & 0x0F;
					hi->mps = mps;
					hi->protocol = current_iface_protocol;
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

	if (dev->interface_count == 0 && !found_hub) {
		print((char*)"Device has no HID interfaces nor hub; disabling slot.");

		// Issue Disable Slot so the xHC stops all activity on this slot and
		// frees its resources.  Without this, the device (e.g. a USB boot
		// drive) is left in the Addressed state with EP0 live.  The xHC can
		// then generate stray Transfer Events (NAKs, STALLs, errors on EP0)
		// that fill the event ring and cause keyboard events to be lost or
		// delayed in the main loop.
		ring_push_cmd(&cr, (10u << 10) | (dev->slot_id << 24), 0, 0);
		doorbell32[0] = 0;
		{
			USB_Response resp;
			for (int i = 0; i < 1000000; i++) {
				resp = get_usb_response(1);
				if (resp.gotresponse && resp.type == 33) break;
			}
		}
		// Clear DCBAA entry so the slot is fully released.
		g_dcbaa[dev->slot_id] = 0;
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
		if (!r.gotresponse) {
			print((char*)"SET_CONFIGURATION failed (may still work on some devices)");
		} else {
			print((char*)"SET_CONFIGURATION ok");
		}
	}

	/* ════════════════════════════════════════════════════════════
	   Hub path — configure the hub's status-change interrupt IN
	   endpoint and register the hub so kmain can queue and service
	   it exactly like a keyboard's interrupt IN endpoint.

	   This mirrors what Linux does in hub_probe() / hub_activate():
	   find the status-change endpoint, submit a persistent interrupt
	   URB (hub_irq()), and re-submit after every completion.  When
	   any downstream port changes state, the hub sends one byte on
	   this endpoint; we restart enumeration in response, cleanly
	   handling both connect and disconnect without any polling or
	   timeout hacks.
	   ════════════════════════════════════════════════════════════ */
	if (found_hub) {
		print((char*)"Found hub; setting up status-change interrupt pipe.");

		// ── Find interrupt IN endpoint in config descriptor ──────
		uint8_t  hub_ep_num  = 0;
		uint16_t hub_ep_mps  = 1;
		uint8_t  hub_ep_ivl  = 0xFF;

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

		// For HS hubs use the bInterval value directly (2^(n-1) * 125 µs
		// microframes); for FS/LS use it as-is in frames.
		uint8_t interval = (dev->speed >= 4) ? 0u : hub_ep_ivl;
		in_hub_ep[0] = ((uint32_t)interval << 16);
		// EP type 7 = Interrupt IN, CErr = 3
		in_hub_ep[1] = (3u << 1) | (7u << 3) | ((uint32_t)hub_ep_mps << 16);
		uint64_t deq = hub_ring->phys | 1u;
		in_hub_ep[2] = (uint32_t)(deq & 0xFFFFFFFFu);
		in_hub_ep[3] = (uint32_t)(deq >> 32);
		in_hub_ep[4] = (uint32_t)hub_ep_mps;

		ring_push_cmd(&cr, (12u << 10) | (dev->slot_id << 24), ictx2_phys, 0);
		doorbell32[0] = 0;

		{
			USB_Response resp;
			for (;;) {
				resp = get_usb_response();
				if (!resp.gotresponse) continue;
				if (resp.type == 33) break;
			}
			uint32_t code = (resp.event->status >> 24) & 0xFF;
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
		USBSetupPacket bp = {};
		bp.bmRequestType = 0x21;
		bp.bRequest      = 0x0B;
		bp.wValue        = 0; // Boot Protocol (if supported)
		bp.wIndex        = i; // Assume interface num = index in simpler devices
		bp.wLength       = 0;
		volatile uint8_t* tmp = (volatile uint8_t*)alloc_table();
		do_control_transfer(*dev, &bp, tmp, 0);
		
		USBSetupPacket si = {};
		si.bmRequestType = 0x21;
		si.bRequest      = 0x0A;
		si.wValue        = 0;
		si.wIndex        = i;
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

	uint8_t interval = (dev->speed >= 4) ? 0u : 3u;

	for (int i = 0; i < dev->interface_count; i++) {
		HIDInterface* hi = &dev->interfaces[i];
		uint8_t dci = hi->ep_num * 2 + 1;
		volatile uint32_t* in_hi_ep = (volatile uint32_t*)((uint64_t)ictx2 + ctx_stride * ((uint64_t)dci + 1u));
		for (int j = 0; j < dws; j++) in_hi_ep[j] = 0;
		in_hi_ep[0] = ((uint32_t)interval << 16);
		in_hi_ep[1] = (3u << 1) | (7u << 3) | ((uint32_t)hi->mps << 16);
		uint64_t deq = hi->ring->phys | 1u;
		in_hi_ep[2] = (uint32_t)(deq & 0xFFFFFFFFu);
		in_hi_ep[3] = (uint32_t)(deq >> 32);
		in_hi_ep[4] = (uint32_t)hi->mps;
	}

	ring_push_cmd(&cr, (12u << 10) | (dev->slot_id << 24), ictx2_phys, 0);
	doorbell32[0] = 0;

	{
		USB_Response resp;
		for (;;) {
			resp = get_usb_response();
			if (!resp.gotresponse) continue;
			if (resp.type == 33) break;
		}
		uint32_t code = (resp.event->status >> 24) & 0xFF;
		if (code != 1) {
			print((char*)"Configure Endpoint failed, code:");
			char str[64];
			to_str(code, str); print(str);
			return;
		}
		print((char*)"Configured IN endpoints for HID interface(s).");
	}

	for (int i = 0; i < dev->interface_count; i++) {
		HIDInterface* hi = &dev->interfaces[i];
		if (hi->type == 1) {
			if (keyboard_count < MAX_KEYBOARDS) keyboards[keyboard_count++] = dev;
		} else {
			if (mouse_count < MAX_MICE) mice[mouse_count++] = dev;
		}
	}
}


/* ── configure_hub_slot ───────────────────────────────────── */
static void configure_hub_slot(USBDevice* hub, uint8_t num_ports, struct Ring& cr) {
	char str[64];
	size_t ctx_stride = (g_hcc1 & (1u << 2)) ? 0x40u : 0x20u;

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

	// Number of Ports lives in DW1[31:24], NOT DW1[15:8].
	// DW1[15:8] is the low byte of Max Exit Latency — corrupting that field
	// causes the hub slot to be mis-configured, making every child Address Device
	// fail with code 17 because the controller has no valid hub state to route
	// transactions through.
	in_slot[1] = (in_slot[1] & ~(0xFFu << 24)) | ((uint32_t)num_ports << 24);

	ring_push_cmd(&cr, (12u << 10) | (hub->slot_id << 24), ictx_phys, 0);
	doorbell32[0] = 0;

	USB_Response resp;
	for (;;) {
		resp = get_usb_response();
		if (resp.gotresponse && resp.type == 33) break;
	}
	uint32_t code = (resp.event->status >> 24) & 0xFF;
	if (code != 1) {
		print((char*)"configure_hub_slot failed, code:");
		to_str(code, str); print(str);
		// Cannot enumerate children if the hub slot is not properly configured.
		needsResetting = true;
	} else {
		print((char*)"Hub slot configured.");
	}
}

/* ── enumerate_hub_children ───────────────────────────────── */
static void enumerate_hub_children(
		USBDevice*           hub,
		volatile XHCIOpRegs* ops,
		struct Ring&         cr,
		uint8_t              max_depth) {

	if (max_depth == 0) return;
	char str[64];

	print((char*)"enumerate_hub_children entered");
	print((char*)"hub slot_id:"); to_str(hub->slot_id, str); print(str);

	/* ── Get hub descriptor (inlined) ── */
	uint8_t hub_desc[16]; for (int i = 0; i < 16; i++) hub_desc[i] = 0;
	{
		volatile uint8_t* buf = (volatile uint8_t*)alloc_table();
		for (int i = 0; i < 4096; i++) buf[i] = 0;

		USBSetupPacket setup;
		setup.bmRequestType = 0xA0;
		setup.bRequest      = 0x06;
		setup.wValue        = (0x29u << 8);
		setup.wIndex        = 0;
		setup.wLength       = 16;

		print((char*)"hub desc: sending control transfer");
		USB_Response r = do_control_transfer(*hub, &setup, buf, 16);
		print((char*)"hub desc: gotresponse="); to_str(r.gotresponse, str); print(str);

		if (!r.gotresponse) {
			print((char*)"FAIL: hub descriptor no response");
			return;
		}

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

	uint8_t num_ports = hub_desc[2];
	print((char*)"Hub num_ports:"); to_str(num_ports, str); print(str);

	if (num_ports == 0 || num_ports > 16) {
		print((char*)"FAIL: implausible num_ports");
		for (int i = 0; i < 8; i++) { to_hex(hub_desc[i], str); print(str); }
		return;
	}

	// Tell the xHCI this slot is a hub before addressing any child.
	configure_hub_slot(hub, num_ports, cr);
	if (needsResetting) return;

	size_t ctx_stride = (g_hcc1 & (1u << 2)) ? 0x40u : 0x20u;

	for (uint8_t p = 1; p <= num_ports; p++) {
		global_port_index++;
		if (portfailed[global_port_index]) {
			print((char*)"Skipping previously failed hub port");
			continue;
		}

		print((char*)"Checking hub port:"); to_str(p, str); print(str);

		// Power the port (feature 8 = PORT_POWER)
		hub_set_port_feature(*hub, p, 8);
		tsc_delay_ms(20);   // 20ms power stabilisation — USB 2.0 spec §11.11

		// Read pre-reset status — verify something is connected
		{
			uint8_t st[4]; for (int i = 0; i < 4; i++) st[i] = 0;
			if (!hub_get_port_status(*hub, p, st)) {
				print((char*)"FAIL: get_port_status (pre-reset) failed");
				portfailed[global_port_index] = true; needsResetting = true; return;
			}
			print((char*)"Pre-reset wPortStatus:");
			for (int i = 0; i < 4; i++) { to_hex(st[i], str); print(str); }

			if (!(st[0] & 1u)) {
				print((char*)"Nothing on this hub port, skipping");
				continue;
			}
		}

		// Clear stale C_PORT_CONNECTION change bit (feature 16)
		hub_clear_port_feature(*hub, p, 16);

		// Request port reset (feature 4 = PORT_RESET)
		print((char*)"Requesting hub port reset");
		if (!hub_set_port_feature(*hub, p, 4)) {
			print((char*)"FAIL: SET_FEATURE PORT_RESET");
			portfailed[global_port_index] = true; needsResetting = true; return;
		}

		// Poll for C_PORT_RESET (wPortChange bit 4) for up to 200ms.
		// 1ms per poll matches Linux hub_port_reset() — no busy spinning.
		bool reset_done = false;
		for (int attempt = 0; attempt < 200; attempt++) {
			tsc_delay_ms(1);   // 1ms per poll, 200ms total budget
			uint8_t st[4]; for (int i = 0; i < 4; i++) st[i] = 0;
			if (!hub_get_port_status(*hub, p, st)) {
				print((char*)"FAIL: get_port_status during reset poll");
				portfailed[global_port_index] = true; needsResetting = true; return;
			}
			if (st[2] & (1u << 4)) {
				reset_done = true;
				print((char*)"Hub port reset complete, attempt:"); to_str(attempt, str); print(str);
				break;
			}
		}

		if (!reset_done) {
			print((char*)"FAIL: hub port reset timed out");
			portfailed[global_port_index] = true; needsResetting = true; return;
		}

		// Clear C_PORT_RESET change bit (feature 20)
		hub_clear_port_feature(*hub, p, 20);

		// USB 2.0 §7.1.7.5: 10ms TRSTRCY recovery after reset before first
		// transaction. The old spin_delay(5000000) was wildly over-budget
		// (potentially seconds); 10ms is the spec minimum and more than enough.
		tsc_delay_ms(10);

		// Read final port status
		uint8_t status4[4]; for (int i = 0; i < 4; i++) status4[i] = 0;
		if (!hub_get_port_status(*hub, p, status4)) {
			print((char*)"FAIL: get_port_status (post-reset)");
			portfailed[global_port_index] = true; needsResetting = true; return;
		}
		print((char*)"Post-reset wPortStatus:");
		for (int i = 0; i < 4; i++) { to_hex(status4[i], str); print(str); }

		if (!(status4[0] & 1u)) {
			print((char*)"PORT_CONNECTION=0 after reset, skipping");
			continue;
		}
		if (!(status4[0] & 2u)) {
			print((char*)"FAIL: PORT_ENABLE=0 after reset");
			portfailed[global_port_index] = true; needsResetting = true; return;
		}

		// Hub port speed bits (USB 2.0 hub spec, wPortStatus):
		//   bit 9  = PORT_LOW_SPEED  → xHCI speed code 2
		//   bit 10 = PORT_HIGH_SPEED → xHCI speed code 3
		//   neither                  → Full Speed → xHCI speed code 1
		uint16_t port_status = (uint16_t)status4[0] | ((uint16_t)status4[1] << 8);
		uint32_t speed;
		if      (port_status & (1u << 9))  speed = 2;   // Low Speed  (xHCI code 2)
		else if (port_status & (1u << 10)) speed = 3;   // High Speed (xHCI code 3)
		else                               speed = 1;   // Full Speed (xHCI code 1)
		print((char*)"Hub child speed:"); to_str(speed, str); print(str);

		// Enable Slot
		print((char*)"Enabling slot for hub child");
		ring_push_cmd(&cr, (9u << 10), 0, 0);
		doorbell32[0] = 0;

		USB_Response resp;
		for (;;) {
			resp = get_usb_response();
			if (resp.gotresponse && resp.type == 33) break;
		}
		uint32_t code    = (resp.event->status >> 24) & 0xFF;
		uint32_t slot_id = (resp.ctrl >> 24) & 0xFF;
		if (code != 1) {
			print((char*)"FAIL: Enable Slot for hub child, code:"); to_str(code, str); print(str);
			portfailed[global_port_index] = true; needsResetting = true; return;
		}
		print((char*)"Hub child slot:"); to_str(slot_id, str); print(str);

		volatile uint64_t* dev_ctx      = (volatile uint64_t*)alloc_table();
		uint64_t           dev_ctx_phys = (uint64_t)dev_ctx - HHDM;
		g_dcbaa[slot_id] = dev_ctx_phys;

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

		uint32_t route_string = (uint32_t)(p & 0xFu);

		slot_ctx[0] = ((speed & 0xFu) << 20) | (1u << 27) | route_string;
		slot_ctx[1] = ((uint32_t)(hub->port_num & 0xFFu) << 16);
		slot_ctx[2] = 0;
		slot_ctx[3] = 0;

		if (hub->speed == 3 && speed < 3) {
			slot_ctx[2] = (hub->slot_id & 0xFFu)
			            | ((uint32_t)(p & 0xFFu) << 8);
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
			if (addr_attempts > 0) {
				// Disable the bad slot
				ring_push_cmd(&cr, (10u << 10) | (slot_id << 24), 0, 0);
				doorbell32[0] = 0;
				for (;;) {
					resp = get_usb_response();
					if (resp.gotresponse && resp.type == 33) break;
				}
				// Exponential backoff: 100ms, 200ms, 300ms...
				// The old spin_delay(500000 * n) was uncalibrated and potentially
				// several seconds per attempt. 100ms per step is plenty.
				tsc_delay_ms(100 * addr_attempts);

				// Enable a fresh slot
				ring_push_cmd(&cr, (9u << 10), 0, 0);
				doorbell32[0] = 0;
				for (;;) {
					resp = get_usb_response();
					if (resp.gotresponse && resp.type == 33) break;
				}
				uint32_t new_code = (resp.event->status >> 24) & 0xFF;
				if (new_code != 1) {
					print((char*)"Re-enable slot failed");
					portfailed[global_port_index] = true; goto next_port;
				}
				slot_id = (resp.ctrl >> 24) & 0xFF;
				g_dcbaa[slot_id] = dev_ctx_phys;
				print((char*)"New slot:"); to_str(slot_id, str); print(str);
			}

			ring_push_cmd(&cr, (11u << 10) | (slot_id << 24), ictx_phys, 0);
			doorbell32[0] = 0;
			for (;;) {
				resp = get_usb_response();
				if (resp.gotresponse && resp.type == 33) break;
			}
			code = (resp.event->status >> 24) & 0xFF;
			if (code == 1) {
				print((char*)"Hub child addressed successfully");
				goto addr_done;
			}
			print((char*)"Address Device failed, code:"); to_str(code, str); print(str);
		}
		portfailed[global_port_index] = true;
		next_port: continue;
		addr_done:

		// USB spec §9.2.6.3: 2ms recovery after SET_ADDRESS before the first
		// descriptor read. The old spin_delay(1000000) was uncalibrated.
		tsc_delay_ms(2);

		USBDevice* child     = (USBDevice*)alloc_table();
		child->slot_id       = (uint8_t)slot_id;
		child->port_num      = p;
		child->root_port_num = 0;
		child->speed         = speed;
		child->ep0_ring      = ep0;
		child->ep0_ring_phys = ep0->phys;
		child->device_ctx    = dev_ctx;
		child->dev_ctx_phys  = dev_ctx_phys;
		child->is_hub        = false;
		child->interface_count = 0;
		child->hub_ring      = nullptr;

		enumerate_device_after_address(child, ops, nullptr, cr, g_hcc1, 0, 0);
		if (needsResetting) return;

		if (child->is_hub) {
			enumerate_hub_children(child, ops, cr, max_depth - 1);
			if (needsResetting) return;
		}
	}
}

/* ── setupUSB ─────────────────────────────────────────────── */
static void setupUSB(uint64_t usb_virt_base, uint8_t usb_start,
					 uint8_t usb_bus, uint8_t usb_dev,
					 uint8_t usb_fn,  uint8_t usb_prog_if) {
	global_port_index = 0;
	needsResetting    = false;

	// Reset keyboard list so a retry pass does not accumulate stale entries
	// from the failed first pass.  The USBDevice objects from a previous pass
	// are orphaned when the controller resets, so they must not be used again.
	keyboard_count = 0;
	for (int k = 0; k < MAX_KEYBOARDS; k++) keyboards[k] = nullptr;
	
	mouse_count = 0;
	for (int m = 0; m < MAX_MICE; m++) mice[m] = nullptr;

	// Reset hub list for the same reason.
	hub_count = 0;
	for (int h = 0; h < MAX_HUBS; h++) {
		hubs[h]     = nullptr;
		hub_dcis[h] = 0;
	}

	char str[64];

	if (usb_prog_if != 0x30) {
		print((char*)"Unsupported USB protocol.");
		hcf();
	}

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

	map_mmio_region(bar_addr, USB_VA_BASE, mmio_size);
	print((char*)"Mapped USB MMIO");

	volatile uint32_t* caps = (volatile uint32_t*)USB_VA_BASE;
	uint32_t info    = caps[0];
	uint32_t caplen  = info & 0xFF;
	uint32_t ver     = (info >> 16) & 0xFFFF;
	uint32_t hcs1    = caps[1];
	uint32_t hcs2    = caps[2];
	uint32_t hcc1    = caps[4];
	uint32_t dboff   = caps[5];
	uint32_t rtsoff  = caps[6];

	g_hcc1 = hcc1;

	bool has_ac64 = (hcc1 & 1u);
	if (!has_ac64) print((char*)"WARNING: xHCI controller is 32-bit only!");

	xhci_legacy_handoff(hcc1, USB_VA_BASE);

	print((char*)"xHCI version:");
	to_str(ver, str); print(str);

	volatile XHCIOpRegs* ops = (volatile XHCIOpRegs*)((uintptr_t)USB_VA_BASE + caplen);

	// Expose ops globally so kmain can compute PORTSC addresses for disconnect
	// detection by polling CCS without relying solely on PSC events.
	g_ops = ops;

	doorbell32             = (volatile uint32_t*)(USB_VA_BASE + (dboff & ~0x3u));
	volatile uint8_t* rt_base = (volatile uint8_t*)(USB_VA_BASE + (rtsoff & ~0x1Fu));

	volatile uint32_t* iman   = (volatile uint32_t*)(rt_base + 0x20 + 0x00);
	volatile uint32_t* erstsz = (volatile uint32_t*)(rt_base + 0x20 + 0x08);
	volatile uint64_t* erstba = (volatile uint64_t*)(rt_base + 0x20 + 0x10);
	erdp                      = (volatile uint64_t*)(rt_base + 0x20 + 0x18);

	// Reset controller
	ops->usbcmd &= ~1u;
	while (!(ops->usbsts & 1u)) {}
	ops->usbcmd |= (1u << 1);
	while (ops->usbcmd & (1u << 1)) {}
	while (ops->usbsts & (1u << 11)) {}

	// Command ring
	struct Ring cr;
	ring_init(&cr);
	ops->crcr = (cr.phys & ~0x3FULL) | 1u;

	// Event ring
	er_virt = (volatile TRB*)alloc_table();
	er_phys = (uint64_t)er_virt - HHDM;
	for (int i = 0; i < ER_RING_SIZE; i++) {
		er_virt[i].parameter = 0;
		er_virt[i].status    = 0;
		er_virt[i].control   = 0;
	}

	volatile ERSTEntry* erst      = (volatile ERSTEntry*)alloc_table();
	uint64_t            erst_phys = (uint64_t)erst - HHDM;
	erst[0].ring_segment_base = er_phys;
	erst[0].ring_segment_size = ER_RING_SIZE;
	erst[0].reserved          = 0;

	*erstsz = 1;
	*erstba = erst_phys;
	*erdp   = er_phys;
	*iman  |= (1u << 1) | (1u << 0);

	ccs        = 1;
	erdp_index = 0;

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

	g_dcbaa = dcbaa_virt;

	ops->dcbaap = dcbaa_phys;
	ops->config = max_slots;

	ops->usbcmd |= 1u;
	while (ops->usbsts & 1u) {}
	ops->crcr = (cr.phys & ~0x3FULL) | 1u;

	print((char*)"xHCI controller running.");

	uint8_t  max_ports  = (hcs1 >> 24) & 0xFF;
	g_max_ports         = max_ports;
	bool     needs_ppc  = (hcc1 & (1u << 3));

	if (needs_ppc) print((char*)"Manual port power required.");

	constexpr uint32_t PORTSC_CCS    = 1u << 0;
	constexpr uint32_t PORTSC_PED    = 1u << 1;
	constexpr uint32_t PORTSC_PR     = 1u << 4;
	constexpr uint32_t PORTSC_PP     = 1u << 9;

	// Flush any PSC events queued during controller start-up — no fixed spin.
	// Linux doesn't wait here either; PORTSC is read directly during port scan.
	// 50ms gives slow firmware time to post initial events before we drain them.
	tsc_delay_ms(50);
	{
		USB_Response resp;
		while ((resp = get_usb_response(1)).gotresponse) {
			if (resp.type == 34) {
				uint32_t port_id = (resp.event->parameter >> 24) & 0xFF;
				print((char*)"Early PSC on port:"); to_str(port_id, str); print(str);
			}
		}
	}

	print((char*)"Scanning root ports...");

	auto port_reset = [&](volatile uint32_t* portsc) -> bool {
		// Assert PR. Zero the W1C change bits (17–22) so a read-modify-write
		// doesn't accidentally clear them — writing 1 to a W1C bit clears it.
		uint32_t val = *portsc;
		val &= ~0x00FE0002u;   // zero: PED(1), CSC(17), PEC(18), WRC(19),
							//       OCC(20), PRC(21), PLC(22), CEC(23)
		val |= PORTSC_PR;
		*portsc = val;

		// Poll PR=0 with up to 100ms (USB 2.0 spec: 50ms reset pulse + margin).
		// We read PORTSC directly — no need to wait for a PSC event.
		for (int ms = 0; ms < 100; ms++) {
			tsc_delay_ms(1);
			uint32_t sc = *portsc;
			if (sc & PORTSC_PR) continue;           // reset still asserted
			if (!(sc & PORTSC_PED)) {
				print((char*)"Port reset: PED=0, device not enabled");
				return false;
			}
			tsc_delay_ms(10);   // TRSTRCY: USB 2.0 §7.1.7.5 — 10ms recovery
			print((char*)"Port enabled.");
			return true;
		}
		print((char*)"Port reset: timed out");
		return false;
	};

	for (uint32_t i = 0; i < max_ports; i++) {
		global_port_index++;
		if (portfailed[global_port_index]) {
			print((char*)"Skipping failed root port");
			continue;
		}

		volatile uint32_t* portsc = (volatile uint32_t*)((uintptr_t)ops + 0x400 + i * 0x10);

		if (needs_ppc) {
			*portsc |= PORTSC_PP;
			tsc_delay_ms(20);   // USB spec: 20ms power stabilisation
		}

		if (!(*portsc & PORTSC_CCS)) continue;

		print((char*)"Device on root port:"); to_str(i + 1, str); print(str);

		if (!port_reset(portsc)) {
			portfailed[global_port_index] = true;
			needsResetting = true;
			return;
		}

		uint32_t speed = (*portsc >> 10) & 0xF;
		print((char*)"Speed:"); to_str(speed, str); print(str);

		// Enable Slot
		ring_push_cmd(&cr, (9u << 10), 0, 0);
		doorbell32[0] = 0;

		USB_Response resp;
		for (;;) {
			resp = get_usb_response();
			if (!resp.gotresponse) continue;
			if (resp.type == 33) break;
			print((char*)"Unexpected event while waiting for slot:");
			to_str(resp.type, str); print(str);
		}

		uint32_t code    = (resp.event->status >> 24) & 0xFF;
		uint32_t slot_id = (resp.ctrl >> 24) & 0xFF;

		if (code != 1) {
			print((char*)"Enable Slot failed:");
			to_str(code, str); print(str);
			portfailed[global_port_index] = true;
			needsResetting = true;
			return;
		}
		print((char*)"Slot:"); to_str(slot_id, str); print(str);

		USBDevice* newdev          = (USBDevice*)alloc_table();
		newdev->slot_id            = (uint8_t)slot_id;
		newdev->port_num           = (uint8_t)(i + 1);
		newdev->root_port_num      = (uint8_t)(i + 1);  // directly on a root port
		newdev->speed              = speed;
		newdev->is_hub             = false;
		newdev->interface_count    = 0;
		newdev->hub_ring           = nullptr;

		struct Ring* ep0           = (struct Ring*)alloc_table();
		ring_init(ep0);
		newdev->ep0_ring           = ep0;
		newdev->ep0_ring_phys      = ep0->phys;

		volatile uint64_t* dev_ctx = (volatile uint64_t*)alloc_table();
		uint64_t dev_ctx_phys      = (uint64_t)dev_ctx - HHDM;
		dcbaa_virt[slot_id]        = dev_ctx_phys;
		newdev->device_ctx         = dev_ctx;
		newdev->dev_ctx_phys       = dev_ctx_phys;

		size_t ctx_stride2 = (hcc1 & (1u << 2)) ? 0x40u : 0x20u;

		volatile uint64_t* ictx      = alloc_table();
		uint64_t           ictx_phys = (uint64_t)ictx - HHDM;
		for (int j = 0; j < 512; j++) ictx[j] = 0;

		volatile InputControlCtx* icc = (volatile InputControlCtx*)ictx;
		icc->drop_flags = 0;
		icc->add_flags  = (1u << 0) | (1u << 1);

		volatile uint32_t* slot_ctx = (volatile uint32_t*)((uint64_t)ictx + ctx_stride2);
		volatile uint32_t* ep0_ctx  = (volatile uint32_t*)((uint64_t)ictx + ctx_stride2 * 2);

		slot_ctx[0] = ((speed & 0xFu) << 20) | (1u << 27);
		slot_ctx[1] = ((uint32_t)(i + 1) & 0xFFu) << 16;

		uint16_t ep0_mps = (speed >= 4) ? 512u : (speed == 3 ? 64u : 8u);
		ep0_ctx[0] = 0;
		ep0_ctx[1] = (3u << 1) | (4u << 3) | ((uint32_t)ep0_mps << 16);
		uint64_t deq = ep0->phys | 1u;
		ep0_ctx[2] = (uint32_t)(deq & 0xFFFFFFFFu);
		ep0_ctx[3] = (uint32_t)(deq >> 32);
		ep0_ctx[4] = 8;

		ring_push_cmd(&cr, (11u << 10) | (slot_id << 24), ictx_phys, 0);
		doorbell32[0] = 0;

		// Wait for Address Device completion (type 33).
		// Count only empty polls against the timeout, not unrelated events
		// (type 34 Port Status Change, type 32 Transfer).
		resp.gotresponse = false;
		{
			int empty_polls = 0;
			while (empty_polls < 2000000) {
				resp = get_usb_response(1);
				if (!resp.gotresponse) { empty_polls++; continue; }
				if (resp.type == 33) break;
				resp.gotresponse = false;
			}
		}

		if (!resp.gotresponse) {
			print((char*)"Timeout waiting for Address Device");
			portfailed[global_port_index] = true;
			needsResetting = true;
			return;
		}

		code = (resp.event->status >> 24) & 0xFF;
		if (code != 1) {
			print((char*)"Address Device failed, code:");
			to_str(code, str); print(str);
			portfailed[global_port_index] = true;
			needsResetting = true;
			return;
		}
		print((char*)"Address Device success.");
		tsc_delay_ms(2);   // USB spec §9.2.6.3: 2ms recovery after SET_ADDRESS

		enumerate_device_after_address(newdev, ops, rt_base, cr, hcc1, hcs1, hcs2);
		if (needsResetting) return;

		if (newdev->is_hub) {
			enumerate_hub_children(newdev, ops, cr, 8);
			if (needsResetting) return;
		}
	}

	print((char*)"Root port enumeration complete.");
}

#endif // usbhelp_h