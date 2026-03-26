#ifndef structures_h
#define structures_h

#include <stdint.h>

// whothefuckknows

// USB HID keycodes to ASCII (unshifted)
static const char hid_to_ascii[256] = {
	0, 0, 0, 0, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
	'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
	'1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
	'\n', 27, '\b', '\t', ' ', '-', '=', '[', ']', '\\', 0, ';', '\'', '`', ',', '.', '/',
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // F-keys
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // more
};

// USB HID keycodes to ASCII (shifted)
static const char hid_to_ascii_shift[256] = {
	0, 0, 0, 0, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L',
	'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
	'!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
	'\n', 27, '\b', '\t', ' ', '_', '+', '{', '}', '|', 0, ':', '"', '~', '<', '>', '?',
};


// pages


struct ACPISDTHeader {
	char     signature[4];
	uint32_t length;
	uint8_t  revision;
	uint8_t  checksum;
	char     oem_id[6];
	char     oem_table_id[8];
	uint32_t oem_revision;
	uint32_t creator_id;
	uint32_t creator_revision;
} __attribute__((packed));

struct MCFGEntry {
	uint64_t base_address;
	uint16_t pci_segment_group;
	uint8_t  start_bus;
	uint8_t  end_bus;
	uint32_t reserved;
} __attribute__((packed));

struct MCFGHeader {
	struct ACPISDTHeader header;
	uint64_t reserved;
	struct MCFGEntry entries[];
} __attribute__((packed));


#define PML4_INDEX(va) (((va) >> 39) & 0x1FF)
#define PDPT_INDEX(va) (((va) >> 30) & 0x1FF)
#define PD_INDEX(va)   (((va) >> 21) & 0x1FF)
#define PT_INDEX(va)   (((va) >> 12) & 0x1FF)

enum PageFlags : uint64_t {
	PRESENT  = 1ULL << 0,
	WRITABLE = 1ULL << 1,
	USER     = 1ULL << 2,
	WRITE_THROUGH = 1ULL << 3,
	CACHE_DISABLE = 1ULL << 4,
	ACCESSED = 1ULL << 5,
	DIRTY    = 1ULL << 6,
	HUGE     = 1ULL << 7,
	GLOBAL   = 1ULL << 8,
	NX       = 1ULL << 63
};

#define PTE_ADDR_MASK     0x000ffffffffff000ULL
#define PD_2M_ADDR_MASK   0x000ffffffe00000ULL
#define PDP_1G_ADDR_MASK  0x000fffffc0000000ULL


// pcie


uint64_t PCI_ECAM_VA_BASE = 0xFFFF'8000'0000'0000ULL;
uint64_t PCI_ECAM_SEG_STRIDE = 0x10000000ULL;

uint64_t PCI_MMIO_VA_BASE = 0xFFFF'9000'0000'0000ULL;
uint64_t USB_VA_BASE = (PCI_MMIO_VA_BASE + 0x0010'0000ULL);


// usb


struct XHCIOpRegs {
	volatile uint32_t usbcmd;
	volatile uint32_t usbsts;
	volatile uint32_t pagesize;
	volatile uint8_t  reserved1[8];
	volatile uint32_t dnctrl;
	volatile uint64_t crcr;
	volatile uint8_t  reserved2[16];
	volatile uint64_t dcbaap;
	volatile uint32_t config;
};

struct TRB {
	uint64_t parameter;
	uint32_t status;
	uint32_t control;
} __attribute__((packed, aligned(16)));

struct ERSTEntry {
	uint64_t ring_segment_base;
	uint32_t ring_segment_size;
	uint32_t reserved;
} __attribute__((packed, aligned(16)));

struct Ring {
	volatile TRB *trb;
	uint64_t phys;
	uint32_t enq;
	uint8_t  pcs;
};

struct InputControlCtx {
	uint32_t drop_flags;
	uint32_t add_flags;
	uint32_t rsvd[6];
};

struct USBSetupPacket {
	uint8_t bmRequestType;
	uint8_t bRequest;
	uint16_t wValue;
	uint16_t wIndex;
	uint16_t wLength;
} __attribute__((packed));

struct USBDevice {
	uint8_t slot_id;
	uint8_t port_num;     // port number on *parent* hub/root (1-based)
	uint8_t root_port_num;   // 1-based root port this device is on; 0 if behind a hub
	uint32_t speed;
	struct Ring* ep0_ring;
	struct Ring* kbd_ring;
	volatile uint64_t* device_ctx;
	bool is_hub;
	bool is_keyboard;
	uint64_t ep0_ring_phys;
	uint64_t dev_ctx_phys;
};

struct USB_Response {
	bool gotresponse;
	TRB* event;
	uint32_t type;
	uint32_t ctrl;
};

// TRB type constants
static constexpr uint32_t TRB_TYPE_SETUP_STAGE  = 2u;
static constexpr uint32_t TRB_TYPE_DATA_STAGE   = 3u;
static constexpr uint32_t TRB_TYPE_STATUS_STAGE = 4u;

#endif