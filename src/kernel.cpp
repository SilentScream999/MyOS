#include <stddef.h>
#include <stdbool.h>

#include "helpers.h"
#include "pagingstuff.h"
#include "usbhelpers.h"

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(4);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request fb_req = {
	.id = LIMINE_FRAMEBUFFER_REQUEST,
	.revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_stack_size_request stack_size_req = {
	.id        = LIMINE_STACK_SIZE_REQUEST,
	.revision  = 0,
	.stack_size = 1024 * 1024
};

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;
__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

extern "C" void kmain(void) {
	if (!LIMINE_BASE_REVISION_SUPPORTED) hcf();
	if (!fb_req.response || fb_req.response->framebuffer_count < 1) hcf();
	
	fb = fb_req.response->framebuffers[0];
	
	if (!memmap_req.response) {
		print((char*)"Error: No memmap response");
		hcf();
	}
	
	if (!hhdm_req.response) {
		print((char*)"Error: No hhdm response");
		hcf();
	}
	
	if (stack_size_req.response == NULL) {
		print((char*)"Error: No didn't increase stack size");
		hcf();
	}
	
	HHDM = hhdm_req.response->offset;
	
	print((char*)"Setting up memory management");
	init_physical_allocator();
	map_hhdm_usable(PRESENT | WRITABLE);
	
	print((char*)"Mapped the entire ram");
	
	if (!rsdp_req.response) hcf();
	uint64_t rsdp_va = (uint64_t)rsdp_req.response->address;
	
	volatile uint8_t *ptr = (volatile uint8_t*)rsdp_va;
	
	const char expected[8] = {'R','S','D',' ','P','T','R',' '};
	bool valid_signature = true;
	for (int i = 0; i < 8; i++) {
		if (ptr[i] != expected[i]) {
			valid_signature = false;
			break;
		}
	}
	
	if (!valid_signature) {
		print((char*)"Error: No valid signature for RSD ptr");
		hcf();
	}
	
	uint32_t rsdt_pa_32 =
		(uint32_t)ptr[0x10] |
		((uint32_t)ptr[0x11] << 8) |
		((uint32_t)ptr[0x12] << 16) |
		((uint32_t)ptr[0x13] << 24);
	
	uint64_t rsdt_va = HHDM+(uint64_t)rsdt_pa_32;
	volatile uint8_t *ptr_rsdt = (volatile uint8_t *)rsdt_va;
	
	const char expected_rsdt[4] = {'R','S','D','T'};
	valid_signature = true;
	for (int i = 0; i < 4; i++) {
		if (ptr_rsdt[i] != expected_rsdt[i]) {
			valid_signature = false;
			break;
		}
	}
	if (!valid_signature) {
		print((char*)"Error: No valid signature for rsdt");
		hcf();
	}
	
	uint32_t rsdt_length =
		(uint32_t)ptr_rsdt[4] |
		((uint32_t)ptr_rsdt[5] << 8) |
		((uint32_t)ptr_rsdt[6] << 16) |
		((uint32_t)ptr_rsdt[7] << 24);
	uint32_t entry_count = (rsdt_length - 36) / 4;
	
	volatile uint8_t *ptr_mcfg = 0;
	
	for (uint32_t i = 0; i < entry_count; i++) {
		uint32_t offset = 36 + i * 4;
	
		uint32_t entry_pa =
			(uint32_t)ptr_rsdt[offset] |
			((uint32_t)ptr_rsdt[offset+1] << 8) |
			((uint32_t)ptr_rsdt[offset+2] << 16) |
			((uint32_t)ptr_rsdt[offset+3] << 24);
	
		uint64_t entry_va = HHDM + (uint64_t)entry_pa;
		volatile uint8_t *entry = (volatile uint8_t *)entry_va;
		
		if (entry[0] == 'M' && entry[1] == 'C' && entry[2] == 'F' && entry[3] == 'G') {
			ptr_mcfg = entry;
			break;
		}
	}
	
	if (ptr_mcfg == 0) {
		print((char*)"Error: ptr_mcfg == 0");
		hcf();
	}
	
	volatile struct MCFGHeader *mcfg = (volatile struct MCFGHeader *)ptr_mcfg;
	
	uint32_t length = mcfg->header.length;
	uint32_t entries = (length - sizeof(struct ACPISDTHeader) - 8) / 16;
	
	uint64_t usb_virt_base;
	uint8_t usb_start;
	uint8_t usb_bus;
	uint8_t usb_dev;
	uint8_t usb_fn;
	uint8_t usb_prog_if;
	bool usb_found = false;
	
	for (uint32_t i = 0; i < entries; i++) {
		auto *e = &mcfg->entries[i];
		
		uint64_t phys_base = e->base_address;
		uint16_t seg  = e->pci_segment_group;
		uint8_t start = e->start_bus;
		uint8_t end   = e->end_bus;
		
		print((char*)"Mapping ECAM");
		
		uint64_t ecam_size = (uint64_t)(end - start + 1) * 0x100000ULL;
		uint64_t virt_base = PCI_ECAM_VA_BASE + (uint64_t)seg * PCI_ECAM_SEG_STRIDE;
		map_ecam_region(phys_base, virt_base, ecam_size);
		print((char*)"Mapped...");
		
		for (uint8_t bus = start; bus <= end; bus++) {
			for (uint8_t dev = 0; dev < 32; dev++) {
				for (uint8_t fn = 0; fn < 8; fn++) {
					uint32_t id = pci_cfg_read32(virt_base, start, bus, dev, fn, 0x00);
					if (id == 0xFFFFFFFF) continue;
					
					uint32_t class_reg = pci_cfg_read32(virt_base, start, bus, dev, fn, 0x08);
					
					uint8_t baseclass = (class_reg >> 24) & 0xFF;
					uint8_t subclass  = (class_reg >> 16) & 0xFF;
					uint8_t prog_if   = (class_reg >> 8) & 0xFF;
					
					if (baseclass == 0x0C && subclass == 0x03) {
						usb_virt_base = virt_base;
						usb_start = start;
						usb_bus = bus;
						usb_dev = dev;
						usb_fn = fn;
						usb_prog_if = prog_if;
						usb_found = true;
						print((char*)"Found a USB controller!");
						break;
					}
				}
				if (usb_found) { break; }
			}
			if (usb_found) { break; }
		}
		if (usb_found) { break; }
	}
	
	if (!usb_found) {
		print((char*)"Could not find a USB controller!");
		hcf();
	}
	
	char str[64];
	to_hex(usb_prog_if, str);
	print((char*)"Prog_If:");
	print(str);
	
	portfailed = (volatile bool*)alloc_table();
	needsResetting = true;
	while (needsResetting) {
		setupUSB(usb_virt_base, usb_start, usb_bus, usb_dev, usb_fn, usb_prog_if);
	}
	
	// Now we should have registered keyboards in keyboards[]
	if (keyboard_count == 0) {
		print((char*)"No keyboard found!");
		hcf();
	}

	print((char*)"About to enter main loop");
	print((char*)"Press any key...");
	print((char*)"Polling for events...");
	
	// Main keyboard polling loop (process events for all keyboards)
	uint8_t last_keys[MAX_KEYBOARDS][6];
	for (int k = 0; k < MAX_KEYBOARDS; k++) for (int j=0;j<6;j++) last_keys[k][j]=0;
	
	uint32_t event_count = 0;
	uint32_t transfer_count = 0;
	uint64_t loop_count = 0;
	
	while (true) {
		loop_count ++;
		
		if (loop_count % 10000000 == 0) {
			print((char*)"Still polling... Events:");
			to_str(event_count, str);
			print(str);
		}
		
		USB_Response resp = get_usb_response(1);
		if (!resp.gotresponse) { continue; }
		
		event_count++;
		if (event_count <= 10) {
			print((char*)"Event received! Type:");
			to_str(resp.type, str);
			print(str);
		}
		
		if (resp.type == 32) {  // Transfer Event
			transfer_count++;
			
			uint32_t code = (resp.event->status >> 24) & 0xFF;
			uint64_t trb_ptr = resp.event->parameter;
			uint32_t transfer_len = resp.event->status & 0xFFFFFF;
			
			if (code == 1 || code == 13) {  // Success or Short Packet
				uint64_t report_phys = trb_ptr;
				volatile uint8_t* report = (volatile uint8_t*)(HHDM + report_phys);
				
				// Find which keyboard owns this buffer (match by buffer pointer)
				int which_kbd = -1;
				for (int k=0;k<keyboard_count;k++) {
					USBDevice *kd = keyboards[k];
					// We compare physical ring base or slot id: simpler: check slot id match with event CTRL if available
					uint32_t slot_from_event = (resp.ctrl >> 24) & 0xFF;
					if (slot_from_event == kd->slot_id) { which_kbd = k; break; }
				}
				if (which_kbd < 0) {
					// fallback: check report contents
					print((char*)"Unknown keyboard transfer event");
					continue;
				}
				
				int k = which_kbd;
				// Parse HID keyboard report
				uint8_t modifiers = report[0];
				bool shift = (modifiers & 0x22) != 0;
				
				// Check for new key presses
				for (int b = 2; b < 8; b++) {
					uint8_t key = report[b];
					if (key == 0) continue;
					
					// Check if this is a new key (not in last_keys)
					bool is_new = true;
					for (int j = 0; j < 6; j++) {
						if (last_keys[k][j] == key) {
							is_new = false;
							break;
						}
					}
					
					if (is_new && key < 256) {
						print((char*)"New key code:");
						to_str(key, str);
						print(str);
						
						char c = shift ? hid_to_ascii_shift[key] : hid_to_ascii[key];
						if (c != 0) {
							print((char*)"Key pressed:");
							char msg[2];
							msg[0] = c;
							msg[1] = '\0';
							print(msg);
						}
					}
				}
				
				// Update last_keys
				for (int kk = 0; kk < 6; kk++) last_keys[k][kk] = report[kk + 2];
				
				// Re-queue TRB on this keyboard's ring using same buffer
				USBDevice *kd = keyboards[k];
				volatile TRB *new_trb = &kd->kbd_ring->trb[kd->kbd_ring->enq];
				new_trb->parameter = report_phys;
				new_trb->status = 8;
				new_trb->control = (1u << 10) | (1u << 5) | kd->kbd_ring->pcs;
				
				if (++kd->kbd_ring->enq == 255) {
					kd->kbd_ring->enq = 0;
					kd->kbd_ring->pcs ^= 1;
				}
				
				// Ring the doorbell for the slot's EP1 (DCI=3)
				doorbell32[kd->slot_id] = 3;
			}
		}
	}
}