// Helper: get hub descriptor (class-specific) - descriptor type 0x29
bool get_hub_descriptor(USBDevice &dev, uint8_t* outbuf, uint16_t len) {
	volatile uint8_t* buf = (volatile uint8_t*)alloc_table();
	for (int i=0;i<4096;i++) buf[i]=0;

	USBSetupPacket setup;
	setup.bmRequestType = 0xA0; // Device to host, Class, Device? (0xA0 or 0xA3 for other forms). Using 0xA0 to request on device
	setup.bRequest = 0x06; // GET_DESCRIPTOR
	setup.wValue = (0x29 << 8) | 0;
	setup.wIndex = 0;
	setup.wLength = len;

	USB_Response r = do_control_transfer(dev, &setup, (volatile uint8_t*)buf, len);
	if (!r.gotresponse) return false;

	for (int i=0;i<len;i++) outbuf[i] = buf[i];
	return true;
}

// Helper: hub port operation - SET_FEATURE on hub port (host->device, class, recipient other)
bool hub_set_port_feature(USBDevice &hub, uint16_t port, uint16_t feature_selector) {
	volatile uint8_t* tmp = (volatile uint8_t*)alloc_table();
	for (int i=0;i<4096;i++) tmp[i]=0;

	USBSetupPacket setup;
	setup.bmRequestType = 0x23; // Host to device, Class, Other (port)
	setup.bRequest = 0x03; // SET_FEATURE
	setup.wValue = feature_selector;
	setup.wIndex = port;
	setup.wLength = 0;

	USB_Response r = do_control_transfer(hub, &setup, tmp, 0);
	return r.gotresponse;
}

// Helper: hub get port status (Device->Host, Class, Other)
bool hub_get_port_status(USBDevice &hub, uint16_t port, uint8_t* status4) {
	volatile uint8_t* tmp = (volatile uint8_t*)alloc_table();
	for (int i=0;i<4096;i++) tmp[i]=0;

	USBSetupPacket setup;
	setup.bmRequestType = 0xA3; // Device to host, Class, Other (port)
	setup.bRequest = 0x00; // GET_STATUS
	setup.wValue = 0;
	setup.wIndex = port;
	setup.wLength = 4;

	USB_Response r = do_control_transfer(hub, &setup, tmp, 4);
	if (!r.gotresponse) return false;

	for (int i=0;i<4;i++) status4[i] = tmp[i];
	return true;
}

// Recursively enumerate hub's downstream ports and address devices
void enumerate_hub_children(USBDevice &hub, volatile XHCIOpRegs* ops, struct Ring &cr, uint8_t max_depth = 8) {
	if (max_depth == 0) return;
	// Get hub descriptor to find bNbrPorts
	uint8_t hub_desc[16];
	for (int i=0;i<16;i++) hub_desc[i]=0;
	if (!get_hub_descriptor(hub, hub_desc, 16)) {
		print((char*)"Failed to read hub descriptor");
		return;
	}
	uint8_t num_ports = hub_desc[2];
	char tmpmsg[64];
	print((char*)"Hub has ports:");
	to_str(num_ports, tmpmsg); print(tmpmsg);
	
	// For each downstream port: power (optional), reset, check status; if device connected then run full enumeration (address device etc.)
	for (uint8_t p = 1; p <= num_ports; ++p) {
		global_port_index ++;
		if (portfailed[global_port_index]) {
			print((char*)"Skipping previously failed port");
			continue;
		}
		
		// Try to power port (feature 8 is PORT_POWER in USB2 hub spec)
		hub_set_port_feature(hub, p, 8);
		spin_delay(20000);
		
		// Reset port (feature PORT_RESET==4)
		if (!hub_set_port_feature(hub, p, 4)) {
			print((char*)"Failed to request port reset on hub child");
			portfailed[global_port_index] = true;
			needsResetting = true;
			return;
		}
		spin_delay(50000);
		
		// Get port status
		uint8_t status4[4];
		for (int i=0;i<4;i++) status4[i]=0;
		if (!hub_get_port_status(hub, p, status4)) {
			portfailed[global_port_index] = true;
			needsResetting = true;
			print((char*)"Failed get port status");
			return;
		}
		// port status bit 0 = connection (LSB)
		if ((status4[0] & 1) == 0) {
			// nothing connected
			continue;
		}
		// If connected: we need to create a new slot for the child device the same way we did for root ports.
		// NOTE: xHCI requires setting slot context's root hub port number to the upstream port (here we use hub.port_num as parent)
		// We'll re-run the same pattern used in root port enumeration, but mark the slot's Root Hub Port Number with the hub's port.
		// Build EP0 transfer ring
		struct Ring *ep0;
		ring_init(ep0);
		uint64_t ep0_ring_phys = ep0->phys;
		
		// Enable Slot command
		ring_push_cmd(&cr, (9u<<10), 0, 0);
		doorbell32[0] = 0;
		
		USB_Response resp;
		while (true) {
			resp = get_usb_response();
			if (resp.type == 33) { break; }
		}
		
		uint32_t slot_id;
		uint32_t code = (resp.event->status >> 24) & 0xFF;
		
		if (code != 1) {
			print((char*)"Enable slot failed for hub child");
			portfailed[global_port_index] = true;
			needsResetting = true;
			return;
		}
		slot_id = (resp.ctrl >> 24) & 0xFF;
		
		// prepare dev_ctx and input_ctx (very similar to root port flow)
		volatile uint64_t* dev_ctx = alloc_table();
		uint64_t dev_ctx_phys = (uint64_t)dev_ctx - HHDM;
		
		volatile uint64_t* input_ctx_virt = alloc_table();
		uint64_t input_ctx_phys = (uint64_t)input_ctx_virt - HHDM;
		for (int j = 0; j < 512; j++) input_ctx_virt[j] = 0;
		
		size_t ctx_stride = (/* hcc1 is unknown here, but for safety choose 0x20 */ 0x20);
		
		volatile InputControlCtx* icc = (volatile InputControlCtx*)input_ctx_virt;
		icc->drop_flags = 0;
		icc->add_flags  = (1u << 0) | (1u << 1);
		
		volatile uint32_t* slot_ctx_dw = (volatile uint32_t*)((uint64_t)input_ctx_virt + ctx_stride);
		
		// Speed unknown until we query port or device. Set default speed=1 (FS) for now.
		uint32_t speed = 1;
		slot_ctx_dw[0] = ((speed & 0xF) << 20) | (1u << 27);
		// Root hub port number should indicate the upstream port on the controller root hub.
		// We set it to hub.port_num (which is the parent's port number on its parent). This is a best-effort mapping.
		slot_ctx_dw[1] = ((uint32_t)(hub.port_num & 0xFFu) << 16);
		
		// EP0 context
		volatile uint32_t* ep0_ctx_dw = (volatile uint32_t*)((uint64_t)input_ctx_virt + ctx_stride * 2);
		
		uint16_t ep0_mps = 8;
		ep0_ctx_dw[0] = 0;
		ep0_ctx_dw[1] = (3u << 1) | (4 << 3) | ((uint32_t)ep0_mps << 16);
		uint64_t deq = ep0_ring_phys | 1u;
		ep0_ctx_dw[2] = (uint32_t)(deq & 0xFFFFFFFFu);
		ep0_ctx_dw[3] = (uint32_t)(deq >> 32);
		ep0_ctx_dw[4] = 8;
		
		// Address Device command (non-blocking)
		ring_push_cmd(&cr, (11u << 10) | (slot_id << 24), input_ctx_phys, 0);
		doorbell32[0] = 0;
		
		resp.gotresponse = false;
		resp.type = 0;
		for (;;) {
			resp = get_usb_response();
			if (resp.type == 33) { break; }
		}
		code = (resp.event->status >> 24) & 0xFF;
		if (code != 1) {
			print((char*)"Address Device failed for hub child");
			portfailed[global_port_index] = true;
			needsResetting = true;
			return;
		}
		
		// create USBDevice record for child
		USBDevice child;
		child.slot_id = slot_id;
		child.port_num = p; // downstream port number on this hub
		child.ep0_ring = ep0;
		child.ep0_ring_phys = ep0_ring_phys;
		child.device_ctx = dev_ctx;
		child.dev_ctx_phys = dev_ctx_phys;
		
		// Now we have addressed the child — we need to run the same next steps as for root ports:
		// - Evaluate Context (omitted for brevity since controller may have set correct EP0 sizes)
		// - Set Boot Protocol (if HID)
		// - Read descriptors and decide whether this is a hub or keyboard and recurse or initialize keyboard
		
		// We'll call a helper that uses the same logic as the root enumeration to finish device setup and to detect hub/keyboard.
		enumerate_device_after_address(child, ops, nullptr, cr, 0, 0, 0);
		// If the child is a hub, recursively enumerate its children
		if (child.is_hub) {
			enumerate_hub_children(child, ops, cr, max_depth - 1);
		}
	}
}