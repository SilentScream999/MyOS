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
	.stack_size = 4 * 1024 * 1024
};

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;
__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

// ── TSC helpers ───────────────────────────────────────────────────────────────
//
// We use the invariant TSC as a wall-clock substitute before any timer driver
// is available — the same technique Linux uses in early boot
// (arch/x86/kernel/tsc.c, tsc_early_delay_calibrate).
//
// rdtsc() returns raw TSC ticks.  get_tsc_freq_hz() figures out how many ticks
// equal one second so we can express repeat thresholds in real milliseconds
// rather than uncalibrated loop iterations.

static inline uint64_t rdtsc() {
	uint32_t lo, hi;
	__asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
}

static uint64_t get_tsc_freq_hz() {
	uint32_t eax, ebx, ecx, edx;

	// CPUID leaf 0x15: TSC / core-crystal ratio (Intel Skylake+).
	// TSC freq = crystal_hz (ecx) * ebx / eax
	__asm__ volatile ("cpuid"
		: "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
		: "a"(0x15u) : );
	if (eax != 0 && ebx != 0 && ecx != 0)
		return (uint64_t)ecx * (uint64_t)ebx / (uint64_t)eax;

	// CPUID leaf 0x16: processor base frequency in MHz (bits 15:0 of eax).
	// Available on Intel Skylake+ when leaf 0x15 has no crystal frequency.
	__asm__ volatile ("cpuid"
		: "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
		: "a"(0x16u) : );
	if ((eax & 0xFFFFu) != 0)
		return (uint64_t)(eax & 0xFFFFu) * 1000000ULL;

	// Last resort: assume 3 GHz.  This is wrong on exotic hardware but will
	// at least produce recognisable (if slightly mistimed) repeat behaviour.
	return 3000000000ULL;
}

extern "C" void kmain(void) {
	if (!LIMINE_BASE_REVISION_SUPPORTED) hcf();
	if (!fb_req.response || fb_req.response->framebuffer_count < 1) hcf();

	fb = fb_req.response->framebuffers[0];

	print((char*)"Stack size req response:");
	if (stack_size_req.response == NULL) {
		print((char*)"NULL - stack NOT increased!");
	} else {
		print((char*)"OK - stack increased");
	}

	if (!memmap_req.response) {
		print((char*)"Error: No memmap response");
		hcf();
	}

	if (!hhdm_req.response) {
		print((char*)"Error: No hhdm response");
		hcf();
	}

	if (stack_size_req.response == NULL) {
		print((char*)"Error: stack size not increased");
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
		if (ptr[i] != expected[i]) { valid_signature = false; break; }
	}
	if (!valid_signature) {
		print((char*)"Error: No valid signature for RSD ptr");
		hcf();
	}

	uint32_t rsdt_pa_32 =
		(uint32_t)ptr[0x10]         |
		((uint32_t)ptr[0x11] <<  8) |
		((uint32_t)ptr[0x12] << 16) |
		((uint32_t)ptr[0x13] << 24);

	uint64_t rsdt_va = HHDM + (uint64_t)rsdt_pa_32;
	volatile uint8_t *ptr_rsdt = (volatile uint8_t *)rsdt_va;

	const char expected_rsdt[4] = {'R','S','D','T'};
	valid_signature = true;
	for (int i = 0; i < 4; i++) {
		if (ptr_rsdt[i] != expected_rsdt[i]) { valid_signature = false; break; }
	}
	if (!valid_signature) {
		print((char*)"Error: No valid signature for rsdt");
		hcf();
	}

	uint32_t rsdt_length =
		(uint32_t)ptr_rsdt[4]         |
		((uint32_t)ptr_rsdt[5] <<  8) |
		((uint32_t)ptr_rsdt[6] << 16) |
		((uint32_t)ptr_rsdt[7] << 24);
	uint32_t entry_count = (rsdt_length - 36) / 4;

	volatile uint8_t *ptr_mcfg = 0;
	for (uint32_t i = 0; i < entry_count; i++) {
		uint32_t offset   = 36 + i * 4;
		uint32_t entry_pa =
			(uint32_t)ptr_rsdt[offset]         |
			((uint32_t)ptr_rsdt[offset+1] <<  8) |
			((uint32_t)ptr_rsdt[offset+2] << 16) |
			((uint32_t)ptr_rsdt[offset+3] << 24);
		uint64_t entry_va   = HHDM + (uint64_t)entry_pa;
		volatile uint8_t *e = (volatile uint8_t *)entry_va;
		if (e[0]=='M' && e[1]=='C' && e[2]=='F' && e[3]=='G') {
			ptr_mcfg = e;
			break;
		}
	}
	if (ptr_mcfg == 0) { print((char*)"Error: ptr_mcfg == 0"); hcf(); }

	volatile struct MCFGHeader *mcfg = (volatile struct MCFGHeader *)ptr_mcfg;
	uint32_t length  = mcfg->header.length;
	uint32_t entries = (length - sizeof(struct ACPISDTHeader) - 8) / 16;

	uint64_t usb_virt_base = 0;
	uint8_t  usb_start = 0, usb_bus = 0, usb_dev = 0, usb_fn = 0, usb_prog_if = 0;
	bool     usb_found = false;

	for (uint32_t i = 0; i < entries; i++) {
		auto *e       = &mcfg->entries[i];
		uint64_t phys_base = e->base_address;
		uint16_t seg       = e->pci_segment_group;
		uint8_t  start     = e->start_bus;
		uint8_t  end       = e->end_bus;

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
					if (((class_reg >> 24) & 0xFF) == 0x0C &&
					    ((class_reg >> 16) & 0xFF) == 0x03) {
						usb_virt_base = virt_base;
						usb_start = start; usb_bus = bus;
						usb_dev = dev;     usb_fn  = fn;
						usb_prog_if = (class_reg >> 8) & 0xFF;
						usb_found   = true;
						print((char*)"Found a USB controller!");
						break;
					}
				}
				if (usb_found) break;
			}
			if (usb_found) break;
		}
		if (usb_found) break;
	}

	if (!usb_found) { print((char*)"Could not find a USB controller!"); hcf(); }

	char str[64];
	to_hex(usb_prog_if, str); print((char*)"Prog_If:"); print(str);

	portfailed = (volatile bool*)alloc_table();

	// ── Outer restart loop ───────────────────────────────────────────────────
	// When the main loop detects a Port Status Change (keyboard disconnected or
	// reconnected), it sets needsResetting=true and breaks.  We jump back here,
	// re-run setupUSB to re-enumerate all ports, reinitialise per-keyboard state,
	// re-queue TRBs, and re-enter the main loop — hot-plug handled correctly.
	restart:
	// Clear portfailed so every port gets a fresh attempt.  Entries set in a
	// previous pass must not carry over — the reconnected keyboard's port would
	// be permanently skipped otherwise.
	for (int _p = 0; _p < 4096; _p++) ((volatile uint8_t*)portfailed)[_p] = 0;

	needsResetting = true;
	while (needsResetting)
		setupUSB(usb_virt_base, usb_start, usb_bus, usb_dev, usb_fn, usb_prog_if);

	if (keyboard_count == 0) {
		print((char*)"No keyboard found after enumeration, waiting for reconnect...");
		// Wait for any PSC event then retry — keyboard may still be connecting.
		for (;;) {
			USB_Response r = get_usb_response(1000000);
			if (r.gotresponse && r.type == 34) goto restart;
		}
	}

	// ── TSC-based repeat thresholds ───────────────────────────────────────────
	//
	// Linux defaults (same values used in drivers/tty/vt/keyboard.c):
	//   repeat_delay  = 250 ms
	//   repeat_period = 40  ms  (25 repeats / sec)
	//
	// We convert to TSC ticks so timing is correct regardless of loop speed.
	uint64_t tsc_hz = get_tsc_freq_hz();

	// Print so you can verify CPUID gave a sensible value.
	print((char*)"TSC freq (Hz):"); to_str(tsc_hz, str); print(str);

	const uint64_t REPEAT_DELAY  = tsc_hz * 250 / 1000;   // 250 ms in ticks
	const uint64_t REPEAT_PERIOD = tsc_hz *  40 / 1000;   //  40 ms in ticks

	// ── Per-keyboard state ────────────────────────────────────────────────────

	uint8_t  last_keys[MAX_KEYBOARDS][6];
	uint8_t  last_mods[MAX_KEYBOARDS];
	for (int k = 0; k < MAX_KEYBOARDS; k++) {
		for (int j = 0; j < 6; j++) last_keys[k][j] = 0;
		last_mods[k] = 0;
	}

	// key_down_at[k][keycode]      = TSC tick when key first went down, 0 if up.
	// last_repeat_fire[k][keycode] = TSC tick of last synthetic repeat emission.
	uint64_t key_down_at[MAX_KEYBOARDS][256];
	uint64_t last_repeat_fire[MAX_KEYBOARDS][256];
	for (int k = 0; k < MAX_KEYBOARDS; k++)
		for (int i = 0; i < 256; i++) {
			key_down_at[k][i]      = 0;
			last_repeat_fire[k][i] = 0;
		}

	print((char*)"About to enter main loop");

	// ── Drain stale events accumulated during enumeration ────────────────────
	// Non-keyboard devices (e.g. a USB boot drive) may have left pending
	// Transfer Events in the ring from their EP0 descriptor reads, or from
	// being left in a confused state after UEFI used them.  Consume and discard
	// everything now so the main loop starts with a clean event ring.
	// We do this BEFORE queueing keyboard TRBs so we can't accidentally
	// discard a real keyboard event.
	{
		USB_Response stale;
		do { stale = get_usb_response(1); } while (stale.gotresponse);
	}

	// ── Queue one initial TRB per keyboard ───────────────────────────────────
	// enumerate_device_after_address intentionally does NOT ring the doorbell
	// or queue a TRB, because any resulting transfer event would be thrown away
	// by the command-completion polling loops inside enumeration.  We do it
	// here, after all enumeration is finished, so every event lands in the
	// main loop below where we can actually handle it.
	for (int k = 0; k < keyboard_count; k++) {
		USBDevice* kd = keyboards[k];

		volatile uint8_t* buf = (volatile uint8_t*)alloc_table();
		for (int j = 0; j < 4096; j++) buf[j] = 0;
		uint64_t buf_phys = (uint64_t)buf - HHDM;

		volatile TRB* trb = &kd->kbd_ring->trb[kd->kbd_ring->enq];
		trb->parameter = buf_phys;
		trb->status    = 8;
		trb->control   = (1u << 10) | (1u << 5) | (kd->kbd_ring->pcs & 1u);

		if (++kd->kbd_ring->enq == LINK_INDEX) {
			kd->kbd_ring->trb[LINK_INDEX].control =
				(TRB_TYPE_LINK << 10) | (1u << 1) | (kd->kbd_ring->pcs & 1u);
			kd->kbd_ring->enq = 0;
			kd->kbd_ring->pcs ^= 1;
		}

		doorbell32[kd->slot_id] = 3;   // Ring EP1 IN (DCI=3)
		print((char*)"Queued initial TRB for keyboard slot:");
		to_str(kd->slot_id, str); print(str);
	}

	print((char*)"Press any key...");

	uint32_t event_count = 0;

	while (true) {

		// ── Process one pending USB event (non-blocking) ──────────────────────
		USB_Response resp = get_usb_response(1);
		if (resp.gotresponse) {
			event_count++;

			// ── Port Status Change (type 34) ─────────────────────────────────
			// A PSC event means something changed on a root port — a device was
			// connected, disconnected, or reset.  We have no hot-plug handler,
			// so the safest response is a full re-enumeration: set needsResetting
			// and break out to the outer while(needsResetting) loop in kmain,
			// which will call setupUSB again and re-discover all keyboards.
			// This handles both disconnect (keyboard gone) and reconnect (keyboard
			// reappears on same or different port) correctly.
			if (resp.type == 34) {
				uint32_t port_id = (resp.event->parameter >> 24) & 0xFF;
				print((char*)"PSC on port during main loop:");
				to_str(port_id, str); print(str);
				needsResetting = true;
				break;
			}

			if (resp.type == 32) {   // Transfer Event
				uint32_t code    = (resp.event->status >> 24) & 0xFF;
				uint64_t trb_ptr = resp.event->parameter;

				if (code == 1 || code == 13) {   // Success or Short Packet

					// trb_ptr is the physical address of the completed TRB.
					// Dereference it to get the actual HID report buffer address.
					volatile TRB*     completed_trb = (volatile TRB*)(HHDM + trb_ptr);
					uint64_t          report_phys   = completed_trb->parameter;
					volatile uint8_t* report        = (volatile uint8_t*)(HHDM + report_phys);

					uint32_t slot_from_event = (resp.ctrl >> 24) & 0xFF;
					int which_kbd = -1;
					for (int k = 0; k < keyboard_count; k++) {
						if (keyboards[k]->slot_id == slot_from_event) { which_kbd = k; break; }
					}

					if (which_kbd >= 0) {
						int     k         = which_kbd;
						uint8_t modifiers = report[0];
						uint64_t now      = rdtsc();

						// ── Mark released keys ────────────────────────────────
						for (int j = 0; j < 6; j++) {
							uint8_t old = last_keys[k][j];
							if (old == 0) continue;
							bool still_held = false;
							for (int b = 2; b < 8; b++) {
								if (report[b] == old) { still_held = true; break; }
							}
							if (!still_held) {
								key_down_at[k][old]      = 0;
								last_repeat_fire[k][old] = 0;
							}
						}

						// ── Emit and timestamp newly-pressed keys ─────────────
						for (int b = 2; b < 8; b++) {
							uint8_t key = report[b];
							if (key == 0) continue;
							if (key_down_at[k][key] == 0) {
								// Fresh press: record TSC time and emit immediately.
								key_down_at[k][key] = now;
								bool shift = (modifiers & 0x22u) != 0;
								char c = shift ? hid_to_ascii_shift[key] : hid_to_ascii[key];
								if (c) {
									char msg[2] = { c, '\0' };
									print((char*)"Key:"); print(msg);
								}
							}
						}

						for (int j = 0; j < 6; j++) last_keys[k][j] = report[2 + j];
						last_mods[k] = modifiers;

						// ── Re-queue transfer TRB ──────────────────────────────
						USBDevice*    kd      = keyboards[k];
						volatile TRB* new_trb = &kd->kbd_ring->trb[kd->kbd_ring->enq];
						new_trb->parameter = report_phys;
						new_trb->status    = 8;
						new_trb->control   = (1u << 10) | (1u << 5) | (kd->kbd_ring->pcs & 1u);

						// Write link TRB with OLD pcs before flipping — if we
						// flip first the controller sees the wrong cycle bit,
						// thinks the ring is empty, and stops after 255 entries.
						if (++kd->kbd_ring->enq == LINK_INDEX) {
							kd->kbd_ring->trb[LINK_INDEX].control =
								(TRB_TYPE_LINK << 10) | (1u << 1) | (kd->kbd_ring->pcs & 1u);
							kd->kbd_ring->enq = 0;
							kd->kbd_ring->pcs ^= 1;
						}

						doorbell32[kd->slot_id] = 3;   // Ring EP1 IN (DCI=3)
					}
				}
			}
		}

		// ── Software key-repeat (mirrors Linux input_repeat_key()) ───────────
		//
		// Runs every iteration so repeat timing tracks wall-clock time via TSC,
		// not USB event arrival rate.
		//
		// Linux behaviour (drivers/input/input.c, input_repeat_key):
		//   1. Key-down  → arm one-shot timer for repeat_delay.
		//   2. Timer fire → emit repeat, re-arm for repeat_period.
		//   3. Key-up    → cancel timer.
		//
		// We replicate phases 1-3 with TSC comparisons:
		//   - key_down_at  tracks when each key went down (TSC ticks).
		//   - last_repeat_fire tracks when we last emitted a synthetic repeat.
		//   - Phase 1: skip if now - key_down_at < REPEAT_DELAY.
		//   - Phase 2: skip if now - last_repeat_fire < REPEAT_PERIOD.
		//   - Phase 3: key-up clears both fields (handled in transfer event above).
		uint64_t now = rdtsc();

		for (int k = 0; k < keyboard_count; k++) {
			bool shift = (last_mods[k] & 0x22u) != 0;

			for (int key = 1; key < 256; key++) {
				uint64_t down_at = key_down_at[k][key];
				if (down_at == 0) continue;   // key is up

				if (now - down_at < REPEAT_DELAY) continue;   // initial delay

				uint64_t fire_at = last_repeat_fire[k][key];
				if (fire_at != 0 && now - fire_at < REPEAT_PERIOD) continue;

				last_repeat_fire[k][key] = now;

				char c = shift ? hid_to_ascii_shift[key] : hid_to_ascii[key];
				if (c) {
					char msg[2] = { c, '\0' };
					print((char*)"Key:"); print(msg);
				}
			}
		}
	}

	// A PSC event caused break — go back and re-enumerate all ports.
	goto restart;
}