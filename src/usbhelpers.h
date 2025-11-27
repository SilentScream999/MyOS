#ifndef usbhelp_h
#define usbhelp_h

#include "structures.h"
#include "pagingstuff.h"
#include "helpers.h"

static volatile uint32_t* doorbell32 = nullptr;

static void ring_init(struct Ring *r) {
	r->trb = (volatile TRB*)alloc_table();
	r->phys = (uint64_t)r->trb - HHDM;
	r->enq = 0;
	r->pcs = 1;

	for (int i = 0; i < 256; i++) r->trb[i].control = 0;

	r->trb[255].parameter = r->phys;
	r->trb[255].status = 0;
	r->trb[255].control = (6u<<10) | (1u<<1);
}

static void ring_push_cmd(struct Ring *r, uint32_t ctrl, uint64_t param, uint32_t status) {
	volatile TRB *t = &r->trb[r->enq];
	t->parameter = param;
	t->status    = status;
	t->control   = (ctrl & ~1u) | r->pcs;

	if (++r->enq == 255) {
		r->enq = 0;
		r->pcs ^= 1;
	}
}

static volatile TRB* er_virt;
static uint8_t ccs = 1; // Consumer Cycle State for event ring
static uint8_t erdp_index = 0;
static volatile uint64_t* erdp;
static uint64_t er_phys;

static USB_Response get_usb_response(int timeout = 1000000) {
	while (timeout-- > 0) {
		TRB* evt = (TRB*)&er_virt[erdp_index];
		uint32_t ctrl = evt->control;
		uint32_t cyc = ctrl & 1u;
		
		if (cyc != ccs) {
			// Event not ready yet
			for (volatile int i = 0; i < 10; i++);
			continue;
		}
		
		// Advance ERDP
		erdp_index = (erdp_index + 1) % 256;
		if (erdp_index == 0) ccs ^= 1;
		uint64_t new_erdp = er_phys + (erdp_index * 16);
		*erdp = new_erdp | (1ull << 3);
		
		// return info
		uint32_t type = (ctrl >> 10) & 0x3F;
		
		USB_Response resp;
		resp.gotresponse = true;
		resp.event = evt;
		resp.type = type;
		resp.ctrl = ctrl;
		return resp;
	}
	
	USB_Response resp;
	resp.gotresponse = false;
	return resp;
}

static USB_Response do_control_transfer(USBDevice &dev,
								 USBSetupPacket* setup,
								 volatile uint8_t* data_buffer,
								 uint16_t data_len) {
	// Encode setup packet as immediate data (little-endian)
	uint64_t setup_dw0 = ((uint64_t)setup->bmRequestType) |
						 ((uint64_t)setup->bRequest   << 8)  |
						 ((uint64_t)setup->wValue     << 16) |
						 ((uint64_t)setup->wIndex     << 32) |
						 ((uint64_t)setup->wLength    << 48);

	// ---- Setup Stage TRB ----
	// status must be 8, control sets: Type, IDT=1, and TRT in bits 17:16
	const uint32_t setup_status = 8u;
	const uint32_t setup_ctrl =
		(TRB_TYPE_SETUP_STAGE << 10) | (1u << 6) | (setup_trt(setup->bmRequestType, setup->wLength) << 16);
	ring_push_cmd(dev.ep0_ring, setup_ctrl, setup_dw0, setup_status);
	
	// ---- Optional Data Stage TRB ----
	const bool has_data = (data_len > 0);
	const bool data_is_in = (setup->bmRequestType & 0x80) != 0;
	
	if (has_data) {
		uint64_t data_phys = (uint64_t)data_buffer - HHDM;

		// Direction bit (bit 16) == 1 for IN, 0 for OUT
		uint32_t data_ctrl = (TRB_TYPE_DATA_STAGE << 10) | (data_is_in ? (1u << 16) : 0u);
		uint32_t data_status = (uint32_t)data_len;  // length goes in "status" dword

		ring_push_cmd(dev.ep0_ring, data_ctrl, data_phys, data_status);
	}
	
	// ---- Status Stage TRB ----
	// Status direction is the OPPOSITE of the request direction
	uint32_t status_dir_bit = data_is_in ? 0u : (1u << 16);  // IN request => OUT status (0), OUT request => IN status (1<<16)
	uint32_t status_ctrl = (TRB_TYPE_STATUS_STAGE << 10) | status_dir_bit | (1u << 5); // IOC on status

	ring_push_cmd(dev.ep0_ring, status_ctrl, 0, 0);
	
	// ---- Ring doorbell for EP0 (DCI=1) ----
	doorbell32[dev.slot_id] = 1;

	// ---- Wait for either Transfer Event (32) or Command Completion (33) ----
	USB_Response resp;
	resp.gotresponse = false;
	for (int j = 0; j < 1000; j++) {
		resp = get_usb_response();
		if (!resp.gotresponse) {
			continue;
		}
		
		if (resp.type == 32 || resp.type == 33) {
			break;
		}
	}
	return resp;
}

static bool get_device_descriptor(USBDevice &dev, volatile uint8_t* outbuf, uint16_t len) {
	volatile uint8_t* buf = (volatile uint8_t*)alloc_table(); // one page for simplicity - already zeroed
	
	USBSetupPacket setup;
	setup.bmRequestType = 0x80; // Device to host, Standard, Device
	setup.bRequest = 0x06; // GET_DESCRIPTOR
	setup.wValue = (0x01 << 8) | 0; // DEVICE descriptor (type=1, index=0)
	setup.wIndex = 0;
	setup.wLength = len;
	
	USB_Response r = do_control_transfer(dev, &setup, buf, len);
	if (!r.gotresponse) return false;
	for (int i=0;i<len;i++) outbuf[i] = buf[i];
	return true;
}

static bool get_configuration_descriptor(USBDevice &dev, volatile uint8_t* outbuf, uint16_t maxlen) {
	volatile uint8_t* buf = (volatile uint8_t*)alloc_table();
	
	USBSetupPacket setup;
	setup.bmRequestType = 0x80;            // Device->Host, Standard, Device
	setup.bRequest      = 0x06;            // GET_DESCRIPTOR
	setup.wValue        = (0x02 << 8) | 0; // CONFIG descriptor (type=2, index=0)
	setup.wIndex        = 0;               // 
	setup.wLength       = maxlen;          // ask for as much as we'll accept
	
	USB_Response r = do_control_transfer(dev, &setup, buf, maxlen);
	if (!r.gotresponse) {
		print((char*)"cfg: no response");
		return false;
	}
	
	uint16_t total_len = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
	if (total_len == 0) {
		print((char*)"cfg: wTotalLength=0");
		return false;
	}
	if (total_len > maxlen) total_len = maxlen;
	
	for (int i = 0; i < total_len; i++) {
		outbuf[i] = buf[i];
	}
	
	return true;
}

#define MAX_KEYBOARDS 8
static  USBDevice* keyboards[MAX_KEYBOARDS];
static  int keyboard_count = 0;

static  volatile bool* portfailed;
static  bool needsResetting = true;
static  int global_port_index;

static void enumerate_device_after_address(USBDevice &dev, volatile XHCIOpRegs* ops, volatile uint8_t* rt_base, struct Ring &cr, uint32_t hcc1, uint32_t hcs1, uint32_t hcs2) {
	print((char*)"Enumerating device.");
	// Create a small buffer and get the device descriptor
	volatile uint8_t* scratch = (volatile uint8_t*)alloc_table(); // pre zeroed
	
	if (!get_device_descriptor(dev, scratch, 8)) {
		print((char*)"Failed to read device descriptor after address");
		return;
	}
	
	char str[64];
	uint8_t mps0 = scratch[7];
	to_str(mps0, str);
	print((char*)"Mps:");
	print(str);
	
	// 1) Context stride: 0x20 (32B) or 0x40 (64B) per context
	size_t ctx_stride = (hcc1 & (1u << 2)) ? 0x40 : 0x20;
	
	// 2) Output Device Context (already set up by Address Device)
	volatile uint64_t* dev_ctx = dev.device_ctx;
	
	// In the *output* device context:
	//   - Slot Context is at index 0
	//   - EP0 Context is at index 1
	volatile uint32_t* out_slot_ctx = (volatile uint32_t*)((uint64_t)dev_ctx + 0 * ctx_stride);
	volatile uint32_t* out_ep0_ctx = (volatile uint32_t*)((uint64_t)dev_ctx + 1 * ctx_stride);
	
	 // 3) Allocate Input Context for Evaluate Context
	volatile uint64_t* input_ctx_virt = alloc_table(); // already cleared
	uint64_t input_ctx_phys = (uint64_t)input_ctx_virt - HHDM;
	// 4) Input Control Context is at the base of the Input Context
	volatile InputControlCtx* icc = (volatile InputControlCtx*)input_ctx_virt;
	icc->drop_flags = 0;
	icc->add_flags  = (1u << 0) | (1u << 1);  // A0 = Slot, A1 = EP0
	
	 // 5) In the *input* context, Slot = context #1, EP0 = context #2
	volatile uint32_t* in_slot_ctx = (volatile uint32_t*)((uint64_t)input_ctx_virt + ctx_stride);
	volatile uint32_t* in_ep0_ctx  = (volatile uint32_t*)((uint64_t)input_ctx_virt + ctx_stride * 2);
	
	// Copy the whole contexts (8 dwords for 32B, 16 for 64B)
	int dw_per_ctx = (int)(ctx_stride / 4);
	for (int d = 0; d < dw_per_ctx; ++d) {
		in_slot_ctx[d] = out_slot_ctx[d];
		in_ep0_ctx[d]  = out_ep0_ctx[d];
	}
	
	// 6) Update EP0 Max Packet Size for Full/Low speed devices
//	if (dev.speed <= 2) {  // 1=FS, 2=LS in your scheme
	uint32_t dw1 = in_ep0_ctx[1];
	// Keep lower 16 bits, replace upper 16 with mps0
	dw1 = (dw1 & 0x0000FFFFu) | ((uint32_t)mps0 << 16);
	in_ep0_ctx[1] = dw1;
//	}
	
	// 7) Issue Evaluate Context (TRB type = 13)
	ring_push_cmd(&cr,
	              (13u << 10) | (dev.slot_id << 24),
	              input_ctx_phys,
	              0);
	// Ring the command ring doorbell (DB0)
	doorbell32[0] = 0;
	
	USB_Response resp;
	for (;;) {
		resp = get_usb_response();
		if (!resp.gotresponse) continue;
		if (resp.type == 33) { // Command Completion Event
			break;
		}
	}
	
	uint8_t code = (resp.event->status >> 24) & 0xFF;
	if (code != 1) {
		print((char*)"Evaluate Context failed, code:");
		to_str(code, str); print(str);
		return;
	}
	
	// Create a small buffer and get the device descriptor
	if (!get_device_descriptor(dev, scratch, 18)) {
		print((char*)"Failed to read device descriptor after address");
		return;
	}
	
	uint8_t dev_class = scratch[4];
	uint8_t dev_subclass = scratch[5];
	uint8_t dev_protocol = scratch[6];
	
	// Get configuration descriptor and parse interfaces
	volatile uint8_t* config_buf = (volatile uint8_t*)alloc_table();
	for (int i=0;i<512;i++) config_buf[i]=0;
	
	if (!get_configuration_descriptor(dev, config_buf, 512)) {
		print((char*)"Failed to read config descriptor");
		// still continue; device may be non-keyboard/hub
	}
	
	// Parse config to find interface descriptors and check for HID Keyboard (interface class 0x03, subclass 1, protocol 1)
	bool found_hid_keyboard = false;
	bool found_hub_device = false;

	// Device descriptor class 0x09 indicates a hub at device level
	if (dev_class == 0x09) {
		found_hub_device = true;
	}
	
	// Search through config_buf for interface descriptors: bDescriptorType==4 (Interface)
	int idx = 0;
	int conf_len = (int)(config_buf[2] | (config_buf[3] << 8));
	while (idx + 2 < conf_len) {
		uint8_t blen = config_buf[idx];
		uint8_t btype = config_buf[idx+1];
		if (blen == 0) break;
		if (btype == 4 && idx + 9 <= conf_len) {
			uint8_t if_class = config_buf[idx+5];
			uint8_t if_sub = config_buf[idx+6];
			uint8_t if_proto = config_buf[idx+7];
			// HID keyboard as boot interface: class=0x03, subclass=1, protocol=1
			if (if_class == 0x03 && if_sub == 0x01 && if_proto == 0x01) {
				found_hid_keyboard = true;
				break;
			}
			// Sometimes device class is 0 (composite), but interface tells us HID.
		}
		if (btype == 0x0F /*Hub class specific? - ignored here*/) {}
		idx += blen;
	}
	
	// Mark device
	dev.is_keyboard = found_hid_keyboard;
	dev.is_hub = found_hub_device || (!found_hid_keyboard && !found_hub_device && false); // if device_class == 9 we already set is_hub

	if (!found_hid_keyboard && !found_hub_device) {
		print((char*)"Device is not keyboard or hub; skipping.");
		return;
	}
	
	if (found_hub_device) {
		// This is a hub. Keep EP0 for hub requests and then do hub-specific initialization.
		dev.is_hub = true;
		print((char*)"Found a hub device.");
		// We'll leave ep0 set up for hub class requests; children enumeration will be triggered externally
		return;
	}
	
	// If it's a keyboard, set Boot Protocol and configure interrupt endpoint (this mirrors original code)
	// Set Boot Protocol (HID-specific)
	volatile uint8_t* setup_buf = (volatile uint8_t*)alloc_table();
	uint64_t setup_phys = (uint64_t)setup_buf - HHDM;
	
	USBSetupPacket* setup = (USBSetupPacket*)setup_buf;
	setup->bmRequestType = 0x21;  // Host to Device, Class, Interface
	setup->bRequest = 0x0B;       // SET_PROTOCOL
	setup->wValue = 0;            // Boot protocol
	setup->wIndex = 0;            // Interface 0
	setup->wLength = 0;
	
	uint64_t setup_dw0 = ((uint64_t)setup->bmRequestType) | 
	                     ((uint64_t)setup->bRequest << 8) |
	                     ((uint64_t)setup->wValue << 16) |
	                     ((uint64_t)setup->wIndex << 32) |
	                     ((uint64_t)setup->wLength << 48);
	
	// push setup and status onto ep0_ring and ring doorbell for slot
	ring_push_cmd(dev.ep0_ring, (2u << 10) | (8 << 16) | (1 << 6), setup_dw0, 0);  // Setup Stage, IDT=1
	// Status Stage TRB (IN direction, IOC=1)
	ring_push_cmd(dev.ep0_ring, (4u << 10) | (1 << 16) | (1 << 5), 0, 0);  // Status IN, IOC

	doorbell32[dev.slot_id] = 1;  // Ring EP0
	
	{
		USB_Response resp;
		for (;;) {
			resp = get_usb_response();
			if (!resp.gotresponse) break;
			if (resp.type == 33 || resp.type == 32) { break; }
		}
		// ignore result here; continue to configure endpoint
	}
	
	// Configure interrupt IN endpoint for keyboard similar to original code:
	struct Ring kbd_ring;
	ring_init(&kbd_ring);
	dev.kbd_ring = &kbd_ring;
	volatile uint64_t* input_ctx2 = alloc_table();
	uint64_t input_ctx2_phys = (uint64_t)input_ctx2 - HHDM;
	for (int j = 0; j < 512; j++) input_ctx2[j] = 0;
	volatile InputControlCtx *icc2 = (volatile InputControlCtx*)input_ctx2;
	icc2->add_flags = (1 << 0) | (1 << 3);  // Add slot context (bit 0) + EP1 IN (DCI=3, bit 3)
	icc2->drop_flags = 0;
	
	// Copy slot context from device output context
	volatile uint32_t* dev_slot_ctx = (volatile uint32_t*)dev.device_ctx;
	volatile uint32_t* slot_ctx2_dw = (volatile uint32_t*)(input_ctx2 + 4);
	for (int j = 0; j < 8; j++) slot_ctx2_dw[j] = dev_slot_ctx[j];
	slot_ctx2_dw[0] = (slot_ctx2_dw[0] & ~(0x1F << 27)) | ((3 & 0x1F) << 27);
	
	// Copy EP0 context from device output context
	volatile uint32_t* dev_ep0_ctx = (volatile uint32_t*)(dev.device_ctx + 4);
	volatile uint32_t* ep0_ctx2_dw = (volatile uint32_t*)(input_ctx2 + 8);
	for (int j = 0; j < 8; j++) ep0_ctx2_dw[j] = dev_ep0_ctx[j];
	
	// EP1 IN context at DCI=3
	volatile uint32_t* ep1_ctx_dw = (volatile uint32_t*)(input_ctx2 + 16);
	// Determine interval based on speed (assume full speed)
	uint8_t interval = 3;
	ep1_ctx_dw[0] = (interval << 16);
	ep1_ctx_dw[1] = (3 << 1) | (7 << 3) | (0 << 8) | (8 << 16);
	ep1_ctx_dw[2] = (uint32_t)(dev.kbd_ring->phys | 1);
	ep1_ctx_dw[3] = (uint32_t)(dev.kbd_ring->phys >> 32);
	ep1_ctx_dw[4] = 8;
	
	// Configure Endpoint command
	ring_push_cmd(&cr, (12u << 10) | (dev.slot_id << 24), input_ctx2_phys, 0);
	doorbell32[0] = 0;

	{
		USB_Response resp;
		resp.gotresponse = false;
		
		for (;;) {
			resp = get_usb_response();
			if (!resp.gotresponse) continue;
			if (resp.type == 33) { break; } // Command Completion Event
		}
		uint32_t code = (resp.event->status >> 24) & 0xFF;
		if (code != 1) {
			print((char*)"Configure endpoint failed for keyboard");
			return;
		}else{
			print((char*)"Configured endpoint for keyboard");
		}
	}
	// Queue initial transfer using ring
	volatile uint8_t* kbd_buffer1 = (volatile uint8_t*)alloc_table();
	volatile uint8_t* kbd_buffer2 = (volatile uint8_t*)alloc_table();
	uint64_t kbd_buffer1_phys = (uint64_t)kbd_buffer1 - HHDM;
	uint64_t kbd_buffer2_phys = (uint64_t)kbd_buffer2 - HHDM;
	for (int j = 0; j < 4096; j++) kbd_buffer1[j] = kbd_buffer2[j] = 0;
	
	volatile TRB *trb1 = &dev.kbd_ring->trb[dev.kbd_ring->enq];
	trb1->parameter = kbd_buffer1_phys;
	trb1->status = 8;
	trb1->control = (1u << 10) | (1u << 5) | dev.kbd_ring->pcs;
	
	if (++dev.kbd_ring->enq == 255) { dev.kbd_ring->enq = 0; dev.kbd_ring->pcs ^= 1; }
	
	doorbell32[dev.slot_id] = 3;  // Ring EP1 IN
	
	// Save into keyboards array if space
	if (keyboard_count < MAX_KEYBOARDS) {
		print((char*)"Putting it into it's place");
		keyboards[keyboard_count++] = &dev;
		print((char*)"Registered a keyboard.");
	} else {
		print((char*)"Keyboard array full; not registering extra keyboard.");
	}
}

static void setupUSB(uint64_t usb_virt_base, uint8_t usb_start, uint8_t usb_bus, uint8_t usb_dev, uint8_t usb_fn, uint8_t usb_prog_if) {
	global_port_index = 0;
	needsResetting = false;
	
	char str[64];
	
	if (usb_prog_if != 0x30) {
		print((char*)"Unsupported USB protocol.");
		hcf();
	}
	
	uint32_t vid_did = pci_cfg_read32(usb_virt_base, usb_start, usb_bus, usb_dev, usb_fn, 0x00);
	uint16_t vendor  = (uint16_t)(vid_did & 0xFFFF);
	
	uint16_t pcmd = pci_cfg_read16(usb_virt_base, usb_start, usb_bus, usb_dev, usb_fn, 0x04);
	pcmd |= (1u<<1) | (1u<<2);
	pci_cfg_write16(usb_virt_base, usb_start, usb_bus, usb_dev, usb_fn, 0x04, pcmd);
	
	if (vendor == 0x8086) {
		print((char*)"Applying Intel xHCI routing...");
		intel_route_all_ports(usb_virt_base, usb_start, usb_bus, usb_dev, usb_fn);
	}
	
	uint32_t bar0 = pci_cfg_read32(usb_virt_base, usb_start, usb_bus, usb_dev, usb_fn, 0x10);

	if ((bar0 & 0x1) == 1) {
		print((char*)"IO space bar err");
		hcf();
	}
	
	uint32_t bar_type = (bar0 >> 1) & 0x3;
	uint64_t bar_addr;
	
	if (bar_type == 0x2) {
		uint32_t bar1_high = pci_cfg_read32(usb_virt_base, usb_start, usb_bus, usb_dev, usb_fn, 0x14);
		bar_addr = ((uint64_t)bar1_high << 32) | (bar0 & ~0xFULL);
	} else {
		bar_addr = bar0 & ~0xFULL;
	}
	
	uint16_t before = pci_cfg_read16(usb_virt_base, usb_start, usb_bus, usb_dev, usb_fn, 0x04);
	pci_cfg_write16(usb_virt_base, usb_start, usb_bus, usb_dev, usb_fn, 0x04, before | (1 << 1));
	uint16_t after = pci_cfg_read16(usb_virt_base, usb_start, usb_bus, usb_dev, usb_fn, 0x04);
	
	pci_cfg_write32(usb_virt_base, usb_start, usb_bus, usb_dev, usb_fn, 0x10, 0xFFFFFFFF);
	uint32_t size_mask = pci_cfg_read32(usb_virt_base, usb_start, usb_bus, usb_dev, usb_fn, 0x10);
	pci_cfg_write32(usb_virt_base, usb_start, usb_bus, usb_dev, usb_fn, 0x10, bar0);
	
	uint64_t mmio_size = ~(size_mask & ~0xF) + 1;
	
	to_str(mmio_size, str);
	print(str);
	to_hex(bar_addr, str);
	print(str);
	
	map_mmio_region(bar_addr, USB_VA_BASE, mmio_size);
	
	print((char*)"Mapped USB");
	
	volatile uint32_t* read = (volatile uint32_t*)USB_VA_BASE;
	
	uint32_t info = read[0];
	
	to_hex(info, str); print(str);
	to_hex((info>>16)&0xFFFF, str); print(str);
	
	uint32_t caplen = (info & 0xFF);
	uint32_t ver = (info>>16) & 0xFFFF;
	uint32_t hcs1 = read[1] & 0xFFFFFFFF;
	uint32_t hcs2 = read[2] & 0xFFFFFFFF;
	uint32_t hcs3 = read[3] & 0xFFFFFFFF;
	uint32_t hcc1 = read[4] & 0xFFFFFFFF;
	uint32_t dboff = read[5] & 0xFFFFFFFF;
	uint32_t rtsoff = read[6] & 0xFFFFFFFF;
	uint32_t hccparams2 = read[7] & 0xFFFFFFFF;
	
	bool has_ac64 = (hcc1 & 1);
	if (!has_ac64) {
		print((char*)"WARNING: xHCI controller is 32-bit only!");
	}
	
	xhci_legacy_handoff(hcc1, USB_VA_BASE);
	
	print((char*)"XHCI found, version: ");
	to_str(ver, str);
	print(str);
	to_str(caplen, str);
	print(str);
	
	volatile XHCIOpRegs* ops = (volatile XHCIOpRegs*)((uintptr_t)USB_VA_BASE + caplen);
	
	doorbell32 = (volatile uint32_t*)(USB_VA_BASE + (dboff & ~0x3));
	volatile uint8_t*  rt_base    = (volatile uint8_t*)(USB_VA_BASE + (rtsoff & ~0x1F));
	
	volatile uint32_t* iman   = (volatile uint32_t*)(rt_base + 0x20 + 0x00);
	volatile uint32_t* imod   = (volatile uint32_t*)(rt_base + 0x20 + 0x04);
	volatile uint32_t* erstsz = (volatile uint32_t*)(rt_base + 0x20 + 0x08);
	volatile uint64_t* erstba = (volatile uint64_t*)(rt_base + 0x20 + 0x10);
	erdp   = (volatile uint64_t*)(rt_base + 0x20 + 0x18);
	
	ops->usbcmd &= ~1u;  // RUN/STOP = 0
	
	while (!(ops->usbsts & 1)) { }
	ops->usbcmd |= (1u << 1);
	while (ops->usbcmd & (1u << 1)) { }
	while (ops->usbsts & (1u << 11)) { }
	
	struct Ring cr;
	ring_init(&cr);
	ops->crcr = (cr.phys & ~0x3FULL) | 1;
	
	er_virt = (volatile TRB*)alloc_table();
	er_phys = ((uint64_t)er_virt) - HHDM;
	for (int i = 0; i < 256; i++) {
		er_virt[i].parameter = 0;
		er_virt[i].status    = 0;
		er_virt[i].control   = 0;
	}
	
	volatile ERSTEntry* erst = (volatile ERSTEntry*)alloc_table();
	uint64_t erst_phys = ((uint64_t)erst) - HHDM;
	
	erst[0].ring_segment_base = er_phys;
	erst[0].ring_segment_size = 256;
	erst[0].reserved          = 0;
	
	*erstsz = 1;
	*erstba = erst_phys;
	*erdp   = er_phys;
	
	*iman |= (1u << 1) | (1u << 0);  // Enable interrupts: IE bit and IP bit
	
	print((char*)"Event ring configured, IMAN:");
	to_hex(*iman, str);
	print(str);
	
	uint32_t max_slots = hcs1 & 0xFF;
	
	volatile uint64_t* dcbaa_virt = (volatile uint64_t*)alloc_table();
	uint64_t dcbaa_phys = ((uint64_t)dcbaa_virt) - HHDM;
	for (uint32_t i = 0; i < max_slots; i++) { dcbaa_virt[i] = 0; }
	
	uint32_t sp_lo = (hcs2 & 0x1F);
	uint32_t sp_hi = (hcs2 >> 27) & 0x1F;
	uint32_t sp_count = (sp_hi << 5) | sp_lo;
	
	if (sp_count) {
		volatile uint64_t* sp_array_virt = (volatile uint64_t*)alloc_table();
		uint64_t sp_array_phys = (uint64_t)sp_array_virt - HHDM;
	
		for (uint32_t i = 0; i < sp_count; i++) {
			volatile uint8_t* page = (volatile uint8_t*)alloc_table();
			uint64_t page_phys = (uint64_t)page - HHDM;
			sp_array_virt[i] = page_phys;
		}
		dcbaa_virt[0] = sp_array_phys;
	}
	
	ops->dcbaap = dcbaa_phys;
	ops->config = max_slots;
	
	ops->usbcmd |= 1u;
	while (ops->usbsts & 1u) { }
	
	ops->crcr = (cr.phys & ~0x3FULL) | 1;
	
	print((char*)"Controller running!");
	
	uint8_t max_ports = (hcs1 >> 24) & 0xFF;
	ccs = 1;
	erdp_index = 0;
	
	bool needs_ppc = (hcc1 & (1u << 3));
	
	if (needs_ppc) {
		print((char*)"We will be powering ports manually.");
	}
	
	print((char*)"Waiting for device connections... (ensure keyboard is plugged in now)");
	size_t timeout = 4000000;
	while (timeout-- > 0) {
		for (volatile int i = 0; i < 1000; i++);
		
		USB_Response resp = get_usb_response(1);
		if (!resp.gotresponse) {
			continue;
		}
		if (resp.type != 34) {
			continue;
		}
		
		// Handle PSC event
		uint32_t port_id = (resp.event->parameter >> 24) & 0xFF;
		print((char*)"Port Status Change on port ");
		to_str(port_id, str); print(str);
		
		volatile uint32_t* portsc = (volatile uint32_t*)((uintptr_t)ops + 0x400 + (port_id-1) * 0x10);
		uint32_t psc = *portsc;
		
		// Check if device connected
		if (psc & 1u) {  // CCS bit
			print((char*)"Device NOW connected!");
			// Clear CSC bit
			*portsc |= (1u << 17);
			// Re-run your port enumeration logic here
		}
	}
	
	print((char*)"Scanning ports...");
	
	constexpr uint32_t PORTSC_CCS = 1u << 0;
	constexpr uint32_t PORTSC_PED = 1u << 1;
	constexpr uint32_t PORTSC_PR  = 1u << 4;
	constexpr uint32_t PORTSC_PLS_MASK = 0xFu << 5;
	constexpr uint32_t PORTSC_PP  = 1u << 9;
	constexpr uint32_t PORTSC_PS_MASK  = 0xFu << 10;
	constexpr uint32_t PORTSC_PIC_MASK = 0x3u << 14;
	constexpr uint32_t PORTSC_LWS = 1u << 16;
	constexpr uint32_t PORTSC_W1C = (1u<<17)|(1u<<18)|(1u<<19)|(1u<<20)|
									(1u<<21)|(1u<<22)|(1u<<23);
	
	auto port_reset = [&](volatile uint32_t* portsc) -> bool {
		uint32_t v_1 = *portsc;
		v_1 |= PORTSC_PR;
		*portsc = v_1;
		
		USB_Response resp;
		while (true) {
			resp = get_usb_response();
			if (resp.gotresponse) {
				break;
			}
		}
		
		if (!resp.gotresponse) {
			print((char*)"Not available.");
			return false;
		}else if (resp.type != 34) {
			print((char*)"Wrong type.");
			return false;
		}
		
		uint32_t v_2 = *portsc;
		if (v_2 & PORTSC_PED) {
			print((char*)"Reset port!");
			return true;
		}else{
			print((char*)"Did not get port reset PED");
			return false;
		}
	};
	
	// root-level enumeration: check each root port and if device present, address and handle device;
	// if hub found, enumerate children recursively
	for (uint32_t i = 0; i < max_ports; i++) {
		global_port_index ++;
		if (portfailed[global_port_index]) {
			print((char*)"Skipping previously failed port");
			continue;
		}
		
		volatile uint32_t* portsc = (volatile uint32_t*)((uintptr_t)ops + 0x400 + i * 0x10);
		
		if (needs_ppc) {
			*portsc |= PORTSC_PP;
			spin_delay(50*1000);
		}
		
		uint32_t v = *portsc;
		if (!(v & PORTSC_CCS)) continue;
		
		print((char*)"");
		
		print((char*)"Device on port ");
		to_str(i+1, str);
		print(str);
		
		if (!port_reset(portsc)) {
			portfailed[global_port_index] = true;
			needsResetting = true;
			return;
		}
		
		// Get port speed (from PORTSC.PSPD) *after* reset
		uint32_t speed = (*portsc >> 10) & 0xF;
		
		print((char*)"Speed:");
		to_str(speed, str); print(str);
		
		// Enable Slot
		ring_push_cmd(&cr, (9u<<10), 0, 0);
		doorbell32[0] = 0;
		
		USB_Response resp;
		while (true) {
			resp = get_usb_response();
			if (!resp.gotresponse) { continue; }
			if (resp.type == 33) {
				resp = resp;
				break;
			}
		}
		
		uint32_t slot_id;
		uint32_t code = (resp.event->status >> 24) & 0xFF;
		
		if (code == 1) {
			slot_id = (resp.ctrl >> 24) & 0xFF;
			print((char*)"Slot enabled! ID=");
			to_str(slot_id, str); 
			print(str);
		} else {
			print((char*)"Enable Slot FAILED with code:");
			portfailed[global_port_index] = true;
			needsResetting = true;
			to_str(code, str);
			print(str);
			return;
		}
		
		struct Ring ep0;
		ring_init(&ep0);
		uint64_t ep0_ring_phys = ep0.phys;
		
		// Allocate device *output* context and hook it into DCBAA
		volatile uint64_t* dev_ctx = alloc_table();          // 4 KiB, aligned
		uint64_t dev_ctx_phys = (uint64_t)dev_ctx - HHDM;
		dcbaa_virt[slot_id] = dev_ctx_phys;
		
		// Allocate input context
		volatile uint64_t* input_ctx_virt = alloc_table();
		uint64_t input_ctx_phys = (uint64_t)input_ctx_virt - HHDM;
		
		// Context stride: 0x20 for 32-byte contexts, 0x40 for 64-byte contexts
		size_t ctx_stride = (hcc1 & (1u << 2)) ? 0x40 : 0x20;
		
		// ---- Input Control Context ----
		volatile InputControlCtx* icc = (volatile InputControlCtx*)input_ctx_virt;
		icc->drop_flags = 0;
		icc->add_flags  = (1u << 0) | (1u << 1);   // A0 = slot, A1 = EP0
		
		// ---- Slot Context ----
		// Slot context starts at 1 * ctx_stride from base of Input Context
		volatile uint32_t* slot_ctx_dw =
			(volatile uint32_t*)((uint64_t)input_ctx_virt + ctx_stride);
		
		// DW0: Route String (0) + Speed + Context Entries
		slot_ctx_dw[0] =
			((speed & 0xF) << 20) |   // Speed
			(1u << 27);               // Context Entries = 1 (only EP0)
		
		// DW1: Max Exit Latency (0) + Root Hub Port Number + Num Ports (0)
		slot_ctx_dw[1] =
			((uint32_t)(i + 1) & 0xFFu) << 16;   // Root Hub Port Number (1-based)
		
		// DW2: TT fields (not a HS hub) = 0
		slot_ctx_dw[2] = 0;
		
		// DW3: Interrupter Target (0 = interrupter 0), addr/slot state = 0
		slot_ctx_dw[3] = 0;
		
		// DW4–DW7: Reserved / unused for simple non-hub device
		slot_ctx_dw[4] = 0;
		slot_ctx_dw[5] = 0;
		slot_ctx_dw[6] = 0;
		slot_ctx_dw[7] = 0;
		
		// ---- EP0 Context ----
		// EP0 context is context #1 -> 2 * ctx_stride from base
		volatile uint32_t* ep0_ctx_dw = (volatile uint32_t*)((uint64_t)input_ctx_virt + ctx_stride * 2);
		
		#define EP_TYPE_CONTROL 4
		
		// EP0 Max Packet Size depends on speed.
		// PSPD default IDs: 1=FS, 2=LS, 3=HS, 4+=SS/SSP.
		// For first Address Device, this is the usual choice:
		uint16_t ep0_mps;
		if (speed >= 4)       // SuperSpeed or better
			ep0_mps = 512;
		else if (speed == 3)  // High Speed
			ep0_mps = 64;
		else                  // Full/Low speed initial guess
			ep0_mps = 8;
		
		// DW0: Endpoint State / Interval / Mult / etc.
		// 0 = Disabled, Mult=0, Interval=0 => fine for initial EP0
		ep0_ctx_dw[0] = 0;
		
		// DW1: CErr (3) + EP Type + Max Burst (0) + Max Packet Size
		ep0_ctx_dw[1] =
			(3u << 1) |                 // CErr = 3 (max error count)
			(EP_TYPE_CONTROL << 3) |    // Endpoint Type = Control
			((uint32_t)ep0_mps << 16);  // Max Packet Size
		
		// DW2–DW3: TR Dequeue Pointer (64-bit) with DCS=1
		uint64_t deq = ep0_ring_phys | 1u;  // ring is 16-byte aligned, so this is valid
		ep0_ctx_dw[2] = (uint32_t)(deq & 0xFFFFFFFFu);
		ep0_ctx_dw[3] = (uint32_t)(deq >> 32);
		
		// DW4: Average TRB Length – 8 bytes is recommended for control endpoints
		ep0_ctx_dw[4] = 8;
		
		// DW5–DW7: reserved
		ep0_ctx_dw[5] = 0;
		ep0_ctx_dw[6] = 0;
		ep0_ctx_dw[7] = 0;
		
		// ---- Address Device Command ----
		ring_push_cmd(&cr, (11u << 10) | (slot_id << 24), input_ctx_phys, 0);
		doorbell32[0] = 0;
		
		
		resp.gotresponse = false;
		for (int j = 0; j < 200; j++) {
			resp = get_usb_response();
			if (!resp.gotresponse) { continue; }
			if (resp.type == 33) { break; } // Command Completion Event
			
			print((char*)"Got a response type in wait for address device:");
			to_str(resp.type, str); print(str);
		}
		
		if (!resp.gotresponse) {
			print((char*)"Timed out waiting for address device response.");
			portfailed[global_port_index] = true;
			needsResetting = true;
			dump_xhci_state(ops, portsc, i, input_ctx_phys);
			return;
		}
		
		code = (resp.event->status >> 24) & 0xFF;
		if (code == 1) {
			print((char*)"Address Device: success");
		} else {
			print((char*)"Address Device failed, code:");
			portfailed[global_port_index] = true;
			needsResetting = true;
			to_str(code, str); print(str);
			return;
		}
		
		spin_delay(10000);  // Give device time to settle
		
		// Create USBDevice for this slot
		USBDevice newdev;
		newdev.slot_id = slot_id;
		newdev.port_num = i+1;
		newdev.speed = speed;
		newdev.ep0_ring = &ep0;
		newdev.device_ctx = dev_ctx;
		newdev.dev_ctx_phys = dev_ctx_phys;
		
		// Determine if this is hub or keyboard and finish setup
		enumerate_device_after_address(newdev, ops, rt_base, cr, hcc1, hcs1, hcs2);
		if (needsResetting) {
			return;
		}

		if (newdev.is_hub) {
			// we're going to skip working on hubs for now
//			enumerate_hub_children(newdev, ops, cr, 8);
//			if (needsResetting) {
//				return;
//			}
		}
	}
	
	print((char*)"");
	print((char*)"Finished root port enumeration.");
}

#endif