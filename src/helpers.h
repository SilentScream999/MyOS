#ifndef helpers_h
#define helpers_h

#include "structures.h"
#include "framebufferstuff.h"

static inline void hcf() {
	for (;;) { __asm__ __volatile__("hlt"); }
}

inline volatile uint32_t* pci_cfg_ptr32(uint64_t virt_base, uint8_t start_bus, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off) {
	if ((off & 3) || off > 0xFFC) hcf();
	uint64_t addr = virt_base
				  + ((uint64_t)(bus - start_bus) << 20)
				  + ((uint64_t)dev << 15)
				  + ((uint64_t)fn  << 12)
				  + off;
	return (volatile uint32_t*)addr;
}

inline volatile uint16_t* pci_cfg_ptr16(uint64_t virt_base, uint8_t start_bus, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off) {
	if ((off & 1) || off > 0xFFE) hcf();
	uint64_t addr = virt_base
				  + ((uint64_t)(bus - start_bus) << 20)
				  + ((uint64_t)dev << 15)
				  + ((uint64_t)fn  << 12)
				  + off;
	return (volatile uint16_t*)addr;
}

uint32_t pci_cfg_read32(uint64_t virt_base, uint8_t start_bus, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off) {
	volatile uint32_t* val = pci_cfg_ptr32(virt_base, start_bus, bus, dev, fn, off);
	return *val;
}

void pci_cfg_write32(uint64_t virt_base, uint8_t start_bus, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off, uint32_t towrite) {
	volatile uint32_t* val = pci_cfg_ptr32(virt_base, start_bus, bus, dev, fn, off);
	*val = towrite;
}

uint16_t pci_cfg_read16(uint64_t virt_base, uint8_t start_bus, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off) {
	volatile uint16_t* val = pci_cfg_ptr16(virt_base, start_bus, bus, dev, fn, off);
	return *val;
}

void pci_cfg_write16(uint64_t virt_base, uint8_t start_bus, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off, uint16_t towrite) {
	volatile uint16_t* val = pci_cfg_ptr16(virt_base, start_bus, bus, dev, fn, off);
	*val = towrite;
}

void to_str(uint64_t value, char* buffer) {
	if (value == 0) {
		buffer[0] = '0';
		buffer[1] = '\0';
		return;
	}
	
	int i = 0;
	while (value > 0) {
		buffer[i] = '0'+(value%10);
		value /= 10;
		i++;
	}
	buffer[i] = '\0';
	
	for (int j = 0; j < i/2; j++) {
		auto temp = buffer[j];
		buffer[j] = buffer[i-j-1];
		buffer[i-j-1] = temp;
	}
}

char HEX_NUMS[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

void to_hex(uint64_t value, char* buffer) {
	int n = 18;
	buffer[n--] = '\0';
	
	for (int i = 0; i < 16; ++i) {
		uint8_t digit = value & 0xF;
		buffer[n--] = HEX_NUMS[digit];
		value >>= 4;
	}
	buffer[n--] = 'x';
	buffer[n--] = '0';
}

static inline uint64_t read_cr3() {
	uint64_t value;
	asm volatile ("mov %%cr3, %0" : "=r"(value));
	return value;
}

static inline void spin_delay(volatile uint64_t iters) {
	for (volatile uint64_t i=0; i<iters; ++i) { __asm__ __volatile__("pause"); }
}

static void xhci_legacy_handoff(uint32_t hcc1, uint64_t mmio_base) {
	uint32_t xecp_dw = (hcc1 >> 16) & 0xFFFF;
	while (xecp_dw) {
		volatile uint32_t *ec = (volatile uint32_t*)(mmio_base + (uint64_t)xecp_dw*4);
		uint32_t hdr   = ec[0];
		uint32_t capid =  hdr        & 0xFF;
		uint32_t next  = (hdr >> 8)  & 0xFF;

		if (capid == 1) {
			volatile uint32_t *usblegsup    = ec + 0;
			volatile uint32_t *usblegctlsts = ec + 1;

			*usblegsup |= (1u << 24);
			for (int t=0; t<1000000 && (*usblegsup & (1u<<16)); ++t) { __asm__ __volatile__("pause"); }

			*usblegctlsts = 0;
			*usblegctlsts = 0xFFFFFFFF;
			break;
		}
		xecp_dw = next;
	}
}

static void intel_route_all_ports(uint64_t virt_base,
								  uint8_t start, uint8_t bus, uint8_t dev, uint8_t fn)
{
	uint32_t xusb2prm = pci_cfg_read32(virt_base, start, bus, dev, fn, 0xD4);
	pci_cfg_write32(virt_base, start, bus, dev, fn, 0xD0, xusb2prm);

	uint32_t usb3_pssen = pci_cfg_read32(virt_base, start, bus, dev, fn, 0xD8);
	pci_cfg_write32(virt_base, start, bus, dev, fn, 0xD8, usb3_pssen);

	uint32_t usb3prm = pci_cfg_read32(virt_base, start, bus, dev, fn, 0xDC);
	pci_cfg_write32(virt_base, start, bus, dev, fn, 0xDC, usb3prm);
}

static inline uint32_t setup_trt(uint8_t bmRequestType, uint16_t wLength) {
	if (wLength == 0) return 0u;                 // No data stage
	return (bmRequestType & 0x80) ? 2u : 3u;     // IN data stage -> 2, OUT -> 3
}

void dump_xhci_state(volatile XHCIOpRegs* ops,
					 volatile uint32_t* portsc,
					 uint32_t port_index,
					 uint64_t input_ctx_phys) {
	volatile uint32_t* usbcmd = (volatile uint32_t*)((uintptr_t)ops + 0x00);
	volatile uint32_t* usbsts = (volatile uint32_t*)((uintptr_t)ops + 0x04);
	volatile uint64_t* crcr   = (volatile uint64_t*)((uintptr_t)ops + 0x18);

	uint32_t cmd = *usbcmd;
	uint32_t sts = *usbsts;
	uint64_t cr  = *crcr;
	uint32_t ps  = *portsc;
	
	char str[64];

	print((char*)"--- xHCI dump ---");
	print((char*)"USBCMD:");  to_hex(cmd, str); print(str);
	print((char*)"USBSTS:");  to_hex(sts, str); print(str);
	print((char*)"CRCR:  ");  to_hex(cr, str); print(str);
	print((char*)"PORTSC[");  to_str(port_index+1, str); print(str); print((char*)"]:"); to_hex(ps, str); print(str);
	print((char*)"InputCtx phys:"); to_hex(input_ctx_phys, str); print(str);
}

#endif