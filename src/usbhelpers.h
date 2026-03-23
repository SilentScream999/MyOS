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

/* ── Keyboard / hub arrays ────────────────────────────────── */
#define MAX_KEYBOARDS 8
static  USBDevice*    keyboards[MAX_KEYBOARDS];
static  int           keyboard_count = 0;

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

	/* ── Step 5: identify hub / HID keyboard ── */
	bool found_keyboard = false;
	bool found_hub      = (dev_class == 0x09);
	uint8_t kbd_iface   = 0;
	uint8_t cfg_val     = config_buf[5];

	int  idx      = 0;
	int  conf_len = (int)((uint16_t)config_buf[2] | ((uint16_t)config_buf[3] << 8));
	while (idx + 2 < conf_len) {
		uint8_t blen  = config_buf[idx];
		uint8_t btype = config_buf[idx + 1];
		if (blen == 0) break;
		if (btype == 4 && idx + 9 <= conf_len) {
			if (config_buf[idx+5] == 0x03 &&
				config_buf[idx+7] == 0x01) {
				found_keyboard = true;
				kbd_iface = config_buf[idx + 2];
				break;
			}
			if (config_buf[idx+5] == 0x09) found_hub = true;
		}
		idx += blen;
	}

	dev->is_keyboard = found_keyboard;
	dev->is_hub      = found_hub;

	if (!found_keyboard && !found_hub) {
		print((char*)"Device is neither keyboard nor hub; disabling slot.");

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
		print((char*)"Slot disabled for non-keyboard device.");
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

	if (found_hub) {
		print((char*)"Found a hub device.");
		return;
	}

	/* ── Keyboard-specific setup ── */

	{
		USBSetupPacket bp = {};
		bp.bmRequestType = 0x21;
		bp.bRequest      = 0x0B;
		bp.wValue        = 0;
		bp.wIndex        = kbd_iface;
		bp.wLength       = 0;
		volatile uint8_t* tmp = (volatile uint8_t*)alloc_table();
		do_control_transfer(*dev, &bp, tmp, 0);
		print((char*)"SET_PROTOCOL (Boot) sent");
	}

	{
		USBSetupPacket si = {};
		si.bmRequestType = 0x21;
		si.bRequest      = 0x0A;
		// wValue high byte = idle duration in 4ms units.
		// 0  = only send on change (we never get USB ticks while key is held).
		// 2  = resend every 8ms while any key is held, giving a reliable
		//      hardware-driven tick for repeat counting without needing a CPU timer.
		si.wValue        = (2u << 8);   // 2 × 4ms = 8ms
		si.wIndex        = kbd_iface;
		si.wLength       = 0;
		volatile uint8_t* tmp = (volatile uint8_t*)alloc_table();
		do_control_transfer(*dev, &si, tmp, 0);
		print((char*)"SET_IDLE sent (8ms interval)");
	}

	/* ── Configure interrupt IN endpoint (EP1 IN, DCI=3) ── */
	struct Ring* kbd_ring = (struct Ring*)alloc_table();
	ring_init(kbd_ring);
	dev->kbd_ring = kbd_ring;

	volatile uint64_t* ictx2      = alloc_table();
	uint64_t           ictx2_phys = (uint64_t)ictx2 - HHDM;
	for (int j = 0; j < 512; j++) ictx2[j] = 0;

	volatile InputControlCtx* icc2 = (volatile InputControlCtx*)ictx2;
	icc2->drop_flags = 0;
	icc2->add_flags  = (1u << 0) | (1u << 3);

	volatile uint32_t* dev_slot_out  = (volatile uint32_t*)((uint64_t)dev->device_ctx);
	volatile uint32_t* dev_ep0_out   = (volatile uint32_t*)((uint64_t)dev->device_ctx + ctx_stride);

	volatile uint32_t* in_slot2 = (volatile uint32_t*)((uint64_t)ictx2 + ctx_stride);
	volatile uint32_t* in_ep02  = (volatile uint32_t*)((uint64_t)ictx2 + ctx_stride * 2);
	volatile uint32_t* in_ep1   = (volatile uint32_t*)((uint64_t)ictx2 + ctx_stride * 4);

	int dws = (int)(ctx_stride / 4);
	for (int j = 0; j < dws; j++) {
		in_slot2[j] = dev_slot_out[j];
		in_ep02[j]  = dev_ep0_out[j];
		in_ep1[j]   = 0;
	}

	in_slot2[0] = (in_slot2[0] & ~(0x1Fu << 27)) | (3u << 27);

	uint8_t interval = (dev->speed >= 4) ? 0u : 3u;
	in_ep1[0] = ((uint32_t)interval << 16);
	in_ep1[1] = (3u << 1) | (7u << 3) | (8u << 16);
	uint64_t deq = kbd_ring->phys | 1u;
	in_ep1[2] = (uint32_t)(deq & 0xFFFFFFFFu);
	in_ep1[3] = (uint32_t)(deq >> 32);
	in_ep1[4] = 8;

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
			to_str(code, str); print(str);
			return;
		}
		print((char*)"Configured EP1 IN for keyboard.");
	}

	// ── Do NOT queue the initial TRB here ─────────────────────────────────
	// Any transfer event generated during enumeration would be silently
	// discarded by the command-completion loops (for(;;) waiting on type 33)
	// that follow this call.  The TRB would be consumed with no re-queue,
	// leaving kmain's main loop waiting forever for an event that never comes.
	// kmain queues one TRB per keyboard after all enumeration is done.

	if (keyboard_count < MAX_KEYBOARDS) {
		keyboards[keyboard_count++] = dev;
		print((char*)"Registered a keyboard.");
	} else {
		print((char*)"Keyboard array full.");
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
	in_slot[0] |= (1u << 26);

	// ── FIX 1 ──────────────────────────────────────────────────────────────────
	// Number of Ports lives in DW1[31:24], NOT DW1[15:8].
	// DW1[15:8] is the low byte of Max Exit Latency — corrupting that field
	// causes the hub slot to be mis-configured, making every child Address Device
	// fail with code 17 because the controller has no valid hub state to route
	// transactions through.
	//
	// Wrong:  (in_slot[1] & ~(0xFFu << 8))  | ((uint32_t)num_ports << 8)
	// Correct:
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
		spin_delay(100000);

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

		// Poll for C_PORT_RESET (wPortChange bit 4) for up to ~200 ms
		bool reset_done = false;
		for (int attempt = 0; attempt < 200; attempt++) {
			spin_delay(10000);
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

		// USB spec requires at least 10 ms TRSTRCY after reset before first
		// transaction.  spin_delay(500000) on a fast host may be under 1 ms;
		// spin_delay(5000000) gives comfortable margin on most hardware.
		spin_delay(5000000);

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

		// ── FIX 2 ──────────────────────────────────────────────────────────────
		// Hub port speed bits (USB 2.0 hub spec, wPortStatus):
		//   bit 9  = PORT_LOW_SPEED  → xHCI speed code 2 (was wrongly mapped to 1)
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

		// ── FIX 3 ──────────────────────────────────────────────────────────────
		// Slot context layout per xHCI spec §6.2.2 Table 57:
		//
		// DW0[19:0]  = Route String: tier-1 nibble = hub's downstream port p
		//              (the root port itself is NOT part of the route string;
		//              it lives in DW1[23:16] as Root Hub Port Number)
		//
		// DW1[23:16] = Root Hub Port Number = hub->port_num  (1-based root port)
		//
		// DW2[15:8]  = TT Port Number      = p   (port on the TT hub)  ← was [7:0]
		// DW2[23:16] = TT Hub Slot ID      = hub->slot_id               ← was correct
		//
		// The child device is NOT a hub itself — do NOT set bit 26 (Hub flag)
		// on its slot context.  Previously slot_ctx[0] |= (1u << 26) was
		// applied here, which told the controller the keyboard was a hub and
		// completely corrupted its routing, producing code 17 on every
		// Address Device attempt.

		uint32_t route_string = (uint32_t)(p & 0xFu);   // tier-1 port only

		slot_ctx[0] = ((speed & 0xFu) << 20) | (1u << 27) | route_string;
		slot_ctx[1] = ((uint32_t)(hub->port_num & 0xFFu) << 16);   // Root Hub Port Number
		slot_ctx[2] = 0;
		slot_ctx[3] = 0;

		if (hub->speed == 3 && speed < 3) {
			// Device needs Transaction Translation through the HS hub.
			// SeaBIOS ground truth (src/hw/xhci.c):
			//   ctx[2] = (tt.port << 8) | tt.hubdev->slotid
			// DW2[7:0]  = TT Hub Slot ID  (slot of the hub providing TT)
			// DW2[15:8] = TT Port Number  (port on that hub the device is on)
			slot_ctx[2] = (hub->slot_id & 0xFFu)          // TT Hub Slot ID → [7:0]
			            | ((uint32_t)(p & 0xFFu) << 8);   // TT Port Number → [15:8]
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
				spin_delay(500000 * addr_attempts);

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

		// Give the device a moment to process the SET_ADDRESS before we
		// start reading descriptors from it.
		spin_delay(1000000);

		USBDevice* child     = (USBDevice*)alloc_table();
		child->slot_id       = (uint8_t)slot_id;
		child->port_num      = p;
		child->speed         = speed;
		child->ep0_ring      = ep0;
		child->ep0_ring_phys = ep0->phys;
		child->device_ctx    = dev_ctx;
		child->dev_ctx_phys  = dev_ctx_phys;
		child->is_hub        = false;
		child->is_keyboard   = false;
		child->kbd_ring      = nullptr;

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
	bool     needs_ppc  = (hcc1 & (1u << 3));

	if (needs_ppc) print((char*)"Manual port power required.");

	constexpr uint32_t PORTSC_CCS    = 1u << 0;
	constexpr uint32_t PORTSC_PED    = 1u << 1;
	constexpr uint32_t PORTSC_PR     = 1u << 4;
	constexpr uint32_t PORTSC_PP     = 1u << 9;

	// Wait briefly for hot-plug events
	{
		size_t timeout = 4000000;
		while (timeout-- > 0) {
			for (volatile int i = 0; i < 1000; i++);
			USB_Response resp = get_usb_response(1);
			if (!resp.gotresponse || resp.type != 34) continue;
			uint32_t port_id = (resp.event->parameter >> 24) & 0xFF;
			print((char*)"Early PSC on port:"); to_str(port_id, str); print(str);
		}
	}

	print((char*)"Scanning root ports...");

	auto port_reset = [&](volatile uint32_t* portsc) -> bool {
		*portsc = (*portsc | PORTSC_PR);
	
		USB_Response resp;
		for (int attempts = 0; attempts < 500; attempts++) {
			resp = get_usb_response(100000);
			if (!resp.gotresponse) continue;
			if (resp.type != 34)  continue;
			if (*portsc & PORTSC_PED) {
				spin_delay(200000);
				print((char*)"Port enabled.");
				return true;
			}
			print((char*)"Port reset: PED not set");
			return false;   // ← bails on the very first PSC regardless of which port it's for
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
			spin_delay(50000);
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
		newdev->speed              = speed;
		newdev->is_hub             = false;
		newdev->is_keyboard        = false;
		newdev->kbd_ring           = nullptr;

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
		// IMPORTANT: count only empty polls against the timeout, not unrelated
		// events (type 34 Port Status Change, type 32 Transfer).  On machines
		// with many ports there can be 6+ PSC events in flight at the same time;
		// if those count against the limit the command times out before its
		// completion event even has a chance to arrive.
		resp.gotresponse = false;
		{
			int empty_polls = 0;
			while (empty_polls < 2000000) {
				resp = get_usb_response(1);
				if (!resp.gotresponse) { empty_polls++; continue; }
				if (resp.type == 33) break;   // got our completion
				// Any other event (PSC, transfer from a different slot, etc.)
				// is silently swallowed — do NOT count it against the timeout.
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
		spin_delay(100000);

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