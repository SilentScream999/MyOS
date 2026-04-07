#include <stddef.h>
#include <stdbool.h>

#include "helpers.h"
#include "pagingstuff.h"
#include "usbhelpers.h"

#include "gdt.h"
#include "idt.h"

#include "irq.h"    // PIC remapping + irq_register
#include "timer.h"  // PIT driver + g_tick_count
#include "heap.h"
#include "tty.h"
#include "acpi.h"
#include "syscall.h"
#include "ramfs.h"
#include "exec.h"
#include "graphics.h"
#include "wm.h"

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(4);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request fb_req = {
	.id = LIMINE_FRAMEBUFFER_REQUEST,
	.revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_req = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_efi_system_table_request efi_st_req = {
    .id = LIMINE_EFI_SYSTEM_TABLE_REQUEST,
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

// ── Syscall globals ───────────────────────────────────────────────────────────
alignas(16) uint8_t g_syscall_kstack[64u * 1024u];
uint64_t g_saved_user_rsp = 0;

// ── klog globals ──────────────────────────────────────────────────────────────
#include "klog.h"
char     g_klog_buffer[KLOG_BUFFER_SIZE];
uint64_t g_klog_pos = 0;
bool     g_klog_bypass_framebuffer = false;
uint8_t* g_backbuffer = nullptr;
uint8_t* g_master_backbuffer = nullptr;

// Phase 11: Speed King globals
uint32_t g_voffset = 0;
volatile uint32_t g_dirty_min_y = 0xFFFFFFFF;
volatile uint32_t g_dirty_max_y = 0;
volatile uint32_t g_dirty_min_x = 0xFFFFFFFF;
volatile uint32_t g_dirty_max_x = 0;
int32_t  g_view_delta = 0;   // rows scrolled up from bottom (0=live, negative=scrolled back)

// Phase 9: WM post-flip hook (set to wm_render_chrome once WM is active)
post_flip_hook_fn g_post_flip_hook = nullptr;
pre_flip_hook_fn  g_pre_flip_hook  = nullptr;
static uint64_t last_mouse_flip_tsc = 0;

// Phase 9: Terminal window origin + size (set by wm_init)
uint32_t g_term_ox = 8;          // fallback: full-screen margin
uint32_t g_term_oy = 8;
uint32_t g_term_max_cols = 80;
uint32_t g_term_max_rows = 24;
uint32_t g_term_history_rows = 0; // lines typed since WM start
uint32_t g_term_total_rows = 0;   // total capacity of backbuffer (rows)
uint32_t g_term_buf_height = 0;   // virtual backbuffer pixel height

int32_t g_mouse_x = 400;
int32_t g_mouse_y = 300;
uint8_t g_mouse_buttons = 0;
uint8_t g_mouse_prev_buttons = 0;
uint64_t tsc_hz = 3000000000ULL; // Calibrated at boot
volatile bool g_needs_refresh = true;
volatile bool wm_chrome_dirty = false;
volatile bool g_dragging_test = false;
volatile bool g_dragging_term = false;
volatile bool g_dragging_rain = false;
volatile uint32_t g_mouse_btns_prev = 0;

#include "mouse.h"

// Phase 18: High-precision TSC calibration against PIT
static void calibrate_tsc_pit() {
    print((char*)"Calibrating TSC...");
    
    // Wait for a PIT tick boundary (g_tick_count from timer.h)
    uint64_t start_tick = g_tick_count;
    while (g_tick_count == start_tick);
    
    // Start measurement
    uint64_t tsc1 = rdtsc();
    uint64_t m_start = g_tick_count;
    
    // Measuring for 100ms (100 ticks) for high precision
    while (g_tick_count < m_start + 100);
    uint64_t tsc2 = rdtsc();
    
    uint64_t delta = tsc2 - tsc1;
    tsc_hz = delta * 10; // 100ms * 10 = 1 second
    
    char mstr[32];
    print((char*)" TSC Hz: "); to_str(tsc_hz, mstr); print(mstr);
}

extern "C" void kmain(void) {
	if (!LIMINE_BASE_REVISION_SUPPORTED) hcf();
	if (!fb_req.response || fb_req.response->framebuffer_count < 1) hcf();

	fb = fb_req.response->framebuffers[0];

    // --- SSE & SIMD Enablement (CR0, CR4) ---
    {
        uint64_t cr0;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        cr0 &= ~(1ULL << 2); // Clear EM (Emulation)
        cr0 |= (1ULL << 1);  // Set MP (Monitor Coprocessor)
        __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");

        uint64_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1ULL << 9);  // Set OSFXSR (FXSAVE/FXRSTOR support)
        cr4 |= (1ULL << 10); // Set OSXMMEXCPT (SIMD exception support)
        __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
    }

	// ── NEW ──────────────────────────────────────
	init_gdt();
	syscall_init();
	print((char*)"GDT loaded.");
	init_idt();
	init_irq();              // remap PIC, mask all IRQ lines
	init_timer(1000);        // PIT at 1000 Hz → 1 ms tick


	__asm__ volatile ("sti"); // enable hardware interrupts
	calibrate_tsc_pit();
	print((char*)"Interrupts enabled. Timer running.");
	// ── END NEW ──────────────────────────────────────

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

	// Disable SMEP and SMAP so kernel can access user-mapped memory.
	{
		uint64_t cr4;
		__asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
		cr4 &= ~(1ULL << 20); // Clear SMEP
		cr4 &= ~(1ULL << 21); // Clear SMAP
		__asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
	}

	print((char*)"Setting up memory management");
	init_physical_allocator();
	map_hhdm_usable(PRESENT | WRITABLE);
	
	// Explicitly map the framebuffer in our new page tables if it's not already covered
	// by the usable RAM map. This ensures sys_write works from user tasks.
	if (fb_req.response && fb_req.response->framebuffer_count > 0) {
		struct limine_framebuffer *f = fb_req.response->framebuffers[0];
		uint64_t phys = (uint64_t)f->address - HHDM;
		uint64_t size = f->pitch * f->height;
		// Map it into the HHDM range explicitly with Write-Combining (WC) enabled via PCD bit.
		// PCD (bit 4) selects PAT entry that is typically WC on modern x86.
		uint64_t flags = PRESENT | WRITABLE | (1ULL << 4); 
		for (uint64_t offset = 0; offset < size; offset += 4096) {
			map_page((uint64_t)f->address + offset, phys + offset, flags);
		}
	}

	init_heap();
    
    // Allocate backbuffer for double buffering + huge scrollback history
    if (fb_req.response && fb_req.response->framebuffer_count > 0) {
        fb = fb_req.response->framebuffers[0];
        
        // 2000 lines @ 10px each = 20,000 pixels high
        g_term_buf_height = 20000;
        if (g_term_buf_height < fb->height) g_term_buf_height = fb->height;
        
        g_backbuffer = (uint8_t*)kmalloc(g_term_buf_height * fb->pitch);
        if (g_backbuffer) {
            memset(g_backbuffer, 0, g_term_buf_height * fb->pitch);
        }

        g_master_backbuffer = (uint8_t*)kmalloc(fb->height * fb->pitch);
        if (g_master_backbuffer) {
            memset(g_master_backbuffer, 0, fb->height * fb->pitch);
            
            g_needs_refresh = true;
        }
    }

	ramfs_init();
	ramfs_mount_klog();
	scheduler_init();

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
	// When the main loop detects a disconnect or Port Status Change, it sets
	// needsResetting=true and breaks.  We jump back here, re-run setupUSB to
	// re-enumerate all ports, reinitialise per-keyboard state, re-queue TRBs,
	// and re-enter the main loop.
	restart:
	// Clear portfailed so every port gets a fresh attempt on each restart.
	for (int _p = 0; _p < 4096; _p++) ((volatile uint8_t*)portfailed)[_p] = 0;

	needsResetting = true;
	while (needsResetting)
		setupUSB(usb_virt_base, usb_start, usb_bus, usb_dev, usb_fn, usb_prog_if);

	// ── Queue hub status-change TRBs BEFORE the keyboard_count == 0 check ───
	//
	// CRITICAL ORDER: hub TRBs must be queued here, before the empty-keyboard
	// wait loop below, not after it.
	//
	// The old code queued hub TRBs after the keyboard TRB loop, which is only
	// reached when keyboard_count > 0.  When an empty hub is plugged in,
	// keyboard_count == 0 so the wait loop fires first — hub TRBs are never
	// queued, the interrupt IN pipe is never armed, and the hub is completely
	// invisible.  It cannot signal downstream connect, disconnect, or anything
	// else, because there is no pending TRB for the xHCI to complete.
	//
	// With hub TRBs queued here:
	//   - Empty hub plug/unplug → Transfer Event on hub slot → wait loop
	//     catches it → goto restart → keyboard found on next pass.
	//   - Hub with keyboard already attached → same path, works identically.
	//   - No hub at all → hub_count == 0, loop body never runs, no cost.
	for (int h = 0; h < hub_count; h++) {
		USBDevice* hd = hubs[h];

		volatile uint8_t* buf = (volatile uint8_t*)alloc_table();
		for (int j = 0; j < 4096; j++) buf[j] = 0;
		uint64_t buf_phys = (uint64_t)buf - HHDM;

		volatile TRB* trb = &hd->hub_ring->trb[hd->hub_ring->enq];
		trb->parameter = buf_phys;
		trb->status    = 1;   // 1 byte status-change bitmap (covers up to 7 ports)
		trb->control   = (1u << 10) | (1u << 5) | (hd->hub_ring->pcs & 1u);

		if (++hd->hub_ring->enq == LINK_INDEX) {
			hd->hub_ring->trb[LINK_INDEX].control =
				(TRB_TYPE_LINK << 10) | (1u << 1) | (hd->hub_ring->pcs & 1u);
			hd->hub_ring->enq = 0;
			hd->hub_ring->pcs ^= 1;
		}

		doorbell32[hd->slot_id] = hub_dcis[h];
		print((char*)"Queued status-change TRB for hub slot:");
		to_str(hd->slot_id, str); print(str);
	}

	print((char*)"[init] Decoupled terminal boot.");
	static bool init_started = false;
	if (!init_started && module_req.response && module_req.response->module_count > 0) {
		struct limine_file *module = module_req.response->modules[0];
		Task* init_task = execve_memory((const uint8_t*)module->address);
		if (init_task) {
			// Phase 9: initialize WM and register post-flip chrome hook
			wm_init();
			g_pre_flip_hook = nullptr; // Phase 18: handled by wm_compose_full
			g_post_flip_hook = nullptr;

			// Re-enable the shell task — it now runs "inside" the WM window
			scheduler_add_task(init_task);
			init_started = true;
			g_klog_bypass_framebuffer = true;
			print((char*)"[wm] Shell task started inside WM frame.");
		} else {
			print((char*)"[init] Failed to load ELF module.");
		}
	}

	// ── Wait for a keyboard ONLY if we need to enumerate first pass ─────────
	// We no longer block indefinitely here. The main loop will handle hotplug.
	if (keyboard_count == 0) {
		print((char*)"No keyboard found yet. Proceeding to background polling...");
	}

	// ── TSC-based repeat thresholds ───────────────────────────────────────────
	//
	// Linux defaults (same values used in drivers/tty/vt/keyboard.c):
	//   repeat_delay  = 250 ms
	//   repeat_period = 40  ms  (25 repeats / sec)
	//
	// We convert to TSC ticks so timing is correct regardless of loop speed.
	// tsc_hz is already calibrated via calibrate_tsc_pit() earlier in kmain.

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
	// discard a real event.
	{
		USB_Response stale;
		do { stale = get_usb_response(1); } while (stale.gotresponse);
	}

	for (int k = 0; k < keyboard_count; k++) {
		USBDevice* kd = keyboards[k];
		for (int i = 0; i < kd->interface_count; i++) {
			HIDInterface* hi = &kd->interfaces[i];
			if (hi->type != 1) continue;

			volatile uint8_t* buf = (volatile uint8_t*)alloc_table();
			for (int j = 0; j < 4096; j++) buf[j] = 0;
			uint64_t buf_phys = (uint64_t)buf - HHDM;

			volatile TRB* trb = &hi->ring->trb[hi->ring->enq];
			trb->parameter = buf_phys;
			trb->status    = (uint32_t)hi->mps;
			trb->control   = (1u << 10) | (1u << 5) | (hi->ring->pcs & 1u);

			if (++hi->ring->enq == LINK_INDEX) {
				hi->ring->trb[LINK_INDEX].control =
					(TRB_TYPE_LINK << 10) | (1u << 1) | (hi->ring->pcs & 1u);
				hi->ring->enq = 0;
				hi->ring->pcs ^= 1;
			}

			uint8_t kbd_dci = hi->ep_num * 2 + 1;
			doorbell32[kd->slot_id] = kbd_dci;
		}
	}

	// ── Queue one initial TRB per mouse ──────────────────────────────────────
	for (int m = 0; m < mouse_count; m++) {
		USBDevice* md = mice[m];
		for (int i = 0; i < md->interface_count; i++) {
			HIDInterface* hi = &md->interfaces[i];
			if (hi->type != 2) continue;

			volatile uint8_t* buf = (volatile uint8_t*)alloc_table();
			for (int j = 0; j < 4096; j++) buf[j] = 0;
			uint64_t buf_phys = (uint64_t)buf - HHDM;

			volatile TRB* trb = &hi->ring->trb[hi->ring->enq];
			trb->parameter = buf_phys;
			trb->status    = (uint32_t)hi->mps;
			trb->control   = (1u << 10) | (1u << 5) | (hi->ring->pcs & 1u);

			if (++hi->ring->enq == LINK_INDEX) {
				hi->ring->trb[LINK_INDEX].control =
					(TRB_TYPE_LINK << 10) | (1u << 1) | (hi->ring->pcs & 1u);
				hi->ring->enq = 0;
				hi->ring->pcs ^= 1;
			}

			uint8_t mouse_dci = hi->ep_num * 2 + 1;
			doorbell32[md->slot_id] = mouse_dci;
		}
	}

	// NOTE: Hub TRBs were already queued before the keyboard_count == 0 block
	// above.  Do NOT queue them again here — doing so would advance enq/pcs
	// and ring the doorbell on an already-armed pipe, producing a duplicate
	// or corrupt TRB that confuses the xHCI.

	print((char*)"Press any key...");

	// Terminal task is now started early, above the repeat threshold calculation.
	// This ensures it survives disconnect/restart cycles.

	uint32_t event_count = 0;

	// --- PHASE 18.5: GUARANTEED FULL-SCREEN SYNC ---
	// Before we enter the reactive loop, ensure the VERY FIRST frame captures the whole screen.
	g_dirty_min_x = 0;
	g_dirty_max_x = fb->width - 1;
	g_dirty_min_y = 0;
	g_dirty_max_y = fb->height - 1;
	g_needs_refresh = true;

	while (true) {
		event_count = 0;
		// ── Drain ALL pending USB events to handle 1000Hz HID traffic ─────────
		while (true) {
			USB_Response resp = get_usb_response(1);
			if (!resp.gotresponse) break;

			event_count++;

			// ── Port Status Change (type 34) ─────────────────────────────────
			// A PSC event means something changed on a root port — a device was
			// connected, disconnected, or reset.  Trigger a full re-enumeration.
			// 300ms settle delay gives PORTSC time to stabilise on same-port
			// reconnects before setupUSB scans it.
			if (resp.type == 34) {
				uint32_t port_id = (resp.event->parameter >> 24) & 0xFF;
				print((char*)"PSC event on port:");
				to_str(port_id, str); print(str);
				tsc_delay_ms(300);
				needsResetting = true;
				break;
			}

			if (resp.type == 32) {   // Transfer Event
				uint32_t code    = (resp.event->status >> 24) & 0xFF;
				uint64_t trb_ptr = resp.event->parameter;
				uint32_t slot_from_event = (resp.ctrl >> 24) & 0xFF;

				// ── Hub status-change interrupt ───────────────────────────────
				//
				// When any downstream port changes state (connect, disconnect,
				// reset complete), the hub fires its interrupt IN endpoint.
				// We get a Transfer Event here on the hub's slot.  Like Linux
				// hub_irq() → schedule hub_event() → hub_port_status(), we
				// restart full enumeration so every changed port is handled.
				//
				// 300ms settle delay before restart so PORTSC is stable on
				// same-port reconnects.
				{
					int which_hub = -1;
					for (int h = 0; h < hub_count; h++) {
						if (hubs[h]->slot_id == slot_from_event) { which_hub = h; break; }
					}

					if (which_hub >= 0 && (code == 1 || code == 13)) {
						print((char*)"Hub status-change interrupt — re-enumerating.");

						// Re-queue the status-change TRB so the pipe stays armed.
						USBDevice* hd     = hubs[which_hub];
						volatile TRB* nxt = &hd->hub_ring->trb[hd->hub_ring->enq];
						nxt->parameter = ((volatile TRB*)(HHDM + trb_ptr))->parameter;
						nxt->status    = 1;
						nxt->control   = (1u << 10) | (1u << 5) | (hd->hub_ring->pcs & 1u);
						if (++hd->hub_ring->enq == LINK_INDEX) {
							hd->hub_ring->trb[LINK_INDEX].control =
								(TRB_TYPE_LINK << 10) | (1u << 1) | (hd->hub_ring->pcs & 1u);
							hd->hub_ring->enq = 0;
							hd->hub_ring->pcs ^= 1;
						}
						doorbell32[hd->slot_id] = hub_dcis[which_hub];

						tsc_delay_ms(300);
						needsResetting = true;
						break;
					}

					// Transfer error on a hub slot → hub itself disconnected.
					if (which_hub >= 0 && code != 1 && code != 13) {
						print((char*)"Hub transfer error, code:");
						to_str(code, str); print(str);
						tsc_delay_ms(300);
						needsResetting = true;
						break;
					}
				}

				if (code == 1 || code == 13) {   // Success or Short Packet

					// trb_ptr is the physical address of the completed TRB.
					// Dereference it to get the actual HID report buffer address.
					volatile TRB*     completed_trb = (volatile TRB*)(HHDM + trb_ptr);
					uint64_t          report_phys   = completed_trb->parameter;
					volatile uint8_t* report        = (volatile uint8_t*)(HHDM + report_phys);

					uint32_t dci_from_event = (resp.ctrl >> 16) & 0x1F;

					int which_kbd = -1;
					HIDInterface* k_hi = nullptr;
					for (int k = 0; k < keyboard_count; k++) {
						if (keyboards[k]->slot_id == slot_from_event) {
							for (int i=0; i<keyboards[k]->interface_count; i++) {
								if (keyboards[k]->interfaces[i].type == 1 && 
									(keyboards[k]->interfaces[i].ep_num*2+1) == dci_from_event) {
									which_kbd = k;
									k_hi = &keyboards[k]->interfaces[i];
									break;
								}
							}
						}
						if (which_kbd >= 0) break;
					}

					int which_mouse = -1;
					HIDInterface* m_hi = nullptr;
					for (int m = 0; m < mouse_count; m++) {
						if (mice[m]->slot_id == slot_from_event) {
							for (int i=0; i<mice[m]->interface_count; i++) {
								if (mice[m]->interfaces[i].type == 2 && 
									(mice[m]->interfaces[i].ep_num*2+1) == dci_from_event) {
									which_mouse = m;
									m_hi = &mice[m]->interfaces[i];
									break;
								}
							}
						}
						if (which_mouse >= 0) break;
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
								if (key >= 0x4F && key <= 0x52) {
									// Shift + Up/Down = Hardware Scrollback
									if (shift && key == 0x52) term_scroll_view(-1);
									else if (shift && key == 0x51) term_scroll_view(1);
									else {
										// Regular Arrow = Shell Navigation
										tty_input('\x1b'); tty_input('[');
										char arrow_codes[] = {'C', 'D', 'B', 'A'};
										tty_input(arrow_codes[key - 0x4F]);
									}
								} else {
									char c = shift ? hid_to_ascii_shift[key] : hid_to_ascii[key];
									if (c) tty_input(c);
								}
							}
						}
						for (int j = 0; j < 6; j++) last_keys[k][j] = report[2 + j];
						last_mods[k] = modifiers;

						// ── Re-queue transfer TRB ──────────────────────────────
						volatile TRB* new_trb = &k_hi->ring->trb[k_hi->ring->enq];
						new_trb->parameter = report_phys;
						new_trb->status    = (uint32_t)k_hi->mps;
						new_trb->control   = (1u << 10) | (1u << 5) | (k_hi->ring->pcs & 1u);

						if (++k_hi->ring->enq == LINK_INDEX) {
							k_hi->ring->trb[LINK_INDEX].control =
								(TRB_TYPE_LINK << 10) | (1u << 1) | (k_hi->ring->pcs & 1u);
							k_hi->ring->enq = 0;
							k_hi->ring->pcs ^= 1;
						}
						doorbell32[slot_from_event] = dci_from_event;
					} else if (which_mouse >= 0) {
						int m = which_mouse;
						bool report_changed = false;
						static uint8_t last_mouse_report[4] = {0,0,0,0};
						for (int i = 0; i < 4; i++) {
							if (last_mouse_report[i] != report[i]) {
								report_changed = true;
								last_mouse_report[i] = report[i];
							}
						}

						// Optimized Heuristic: 
						// 1. Shift if explicitly Report Protocol (0).
						// 2. Shift if a composite device (I > 1) with small packets (MPS 8, likely Trackpad)
						//    and common Report IDs (0x01/0x02). This ignores gaming mice (MPS 32/64).
						uint8_t* data = (uint8_t*)report;
						bool shifted = false;
						if (m_hi->protocol == 0 || (mice[m]->interface_count > 1 && m_hi->mps == 8 && (report[0] == 0x01 || report[0] == 0x02))) {
							data++;
							shifted = true;
						}

						// --- PHASE 18.5: CLEAN UNIFIED DIRTY TRACKING ---
						// The cursor never touches dirty rects. It goes straight to VRAM via
						// wm_overlay_update() at the bottom of this block. The compositor only
						// runs when scene content changes, not for cursor movement or dragging.
						{
							int8_t dx = (int8_t)data[1];
							int8_t dy = (int8_t)data[2];

							g_mouse_prev_buttons = g_mouse_buttons;
							g_mouse_buttons      = data[0];
							bool btn_held     = (g_mouse_buttons  & 1);
							bool btn_was_held = (g_mouse_prev_buttons & 1);
							bool mouse_press  = btn_held && !btn_was_held;
							bool mouse_release= !btn_held && btn_was_held;

							// 1. Move cursor (clamped to screen)
							g_mouse_x += dx;
							g_mouse_y += dy;
							if (g_mouse_x < 0) g_mouse_x = 0;
							if (g_mouse_x >= (int32_t)fb->width)  g_mouse_x = fb->width  - 1;
							if (g_mouse_y < 0) g_mouse_y = 0;
							if (g_mouse_y >= (int32_t)fb->height) g_mouse_y = fb->height - 1;

							// 2. Start drag / Raise / Close / Taskbar: hit-test Z-stack on fresh click
							if (mouse_press) {
								int32_t mx = g_mouse_x, my = g_mouse_y;
								
								// Taskbar / Start button
								if (my >= (int32_t)fb->height - (int32_t)g_taskbar_h) {
									if (mx >= 6 && mx <= 98) {
										// Start button
										term_clear_screen();
										term_write_string((char*)"[SYSTEM] Start Menu coming soon...\r\n");
									} else {
										// Task buttons — mirror the draw layout exactly
										const int32_t WBTN_W = 124, WBTN_GAP = 4;
										int32_t cur_x = 6 + 92 + 10;  // sb_x + sb_w + gap
										for (int idx = 0; idx < 3; idx++) {
											if (!g_win_visible[idx]) { continue; }
											if (mx >= cur_x && mx < cur_x + WBTN_W) {
												wm_raise_window(idx);
												break;
											}
											cur_x += WBTN_W + WBTN_GAP;
										}
									}
								} else {
									int idx = wm_find_top_window(mx, my);
									if (idx != -1) {
										wm_raise_window(idx); // Move to top
										if (wm_is_in_close(idx, mx, my)) {
											wm_close_window(idx);
										} else if (wm_is_in_title(idx, mx, my)) {
											// Start dragging the topmost window
											if (idx == WIN_TERM) {
												wm_ghost_dragging_term = true; g_dragging_term = true;
												wm_drag_term_offx = mx - (int32_t)g_term_ox;
												wm_drag_term_offy = my - (int32_t)g_term_oy;
											} else if (idx == WIN_TEST) {
												wm_ghost_dragging_test = true; g_dragging_test = true;
												wm_drag_test_offx = mx - test_x;
												wm_drag_test_offy = my - test_y;
											} else if (idx == WIN_RAIN) {
												wm_ghost_dragging_rain = true; g_dragging_rain = true;
												wm_drag_rain_offx = mx - rain_x;
												wm_drag_rain_offy = my - rain_y;
											}
										}
									}
								}
							}

							// 3. Update ghost position while dragging (zero cost — just two int adds)
							if (wm_ghost_dragging_term) {
								wm_ghost_term_ox = g_mouse_x - wm_drag_term_offx;
								wm_ghost_term_oy = g_mouse_y - wm_drag_term_offy;
							}
							if (wm_ghost_dragging_test) {
								wm_ghost_test_ox = g_mouse_x - wm_drag_test_offx;
								wm_ghost_test_oy = g_mouse_y - wm_drag_test_offy;
							}
							if (wm_ghost_dragging_rain) {
								wm_ghost_rain_ox = g_mouse_x - wm_drag_rain_offx;
								wm_ghost_rain_oy = g_mouse_y - wm_drag_rain_offy;
							}

							// 4. Commit on release: NOW the window actually moves, triggering one recompose
							if (mouse_release) {
								if (wm_ghost_dragging_term) {
									g_term_ox = (uint32_t)wm_ghost_term_ox;
									g_term_oy = (uint32_t)wm_ghost_term_oy;
									wm_ghost_dragging_term = false;
									g_dragging_term        = false;
									// Full screen dirty: old location + new location both need repaint
									g_dirty_min_x = 0; g_dirty_max_x = fb->width  - 1;
									g_dirty_min_y = 0; g_dirty_max_y = fb->height - 1;
									g_needs_refresh = true;
								}
								if (wm_ghost_dragging_test) {
									test_x = wm_ghost_test_ox;
									test_y = wm_ghost_test_oy;
									wm_ghost_dragging_test = false;
									g_dragging_test        = false;
									g_dirty_min_x = 0; g_dirty_max_x = fb->width  - 1;
									g_dirty_min_y = 0; g_dirty_max_y = fb->height - 1;
									g_needs_refresh = true;
								}
								if (wm_ghost_dragging_rain) {
									rain_x = wm_ghost_rain_ox;
									rain_y = wm_ghost_rain_oy;
									wm_ghost_dragging_rain = false;
									g_dragging_rain        = false;
									g_dirty_min_x = 0; g_dirty_max_x = fb->width  - 1;
									g_dirty_min_y = 0; g_dirty_max_y = fb->height - 1;
									g_needs_refresh = true;
								}
							}

							// Re-queue transfer TRB
							volatile TRB* new_trb = &m_hi->ring->trb[m_hi->ring->enq];
							new_trb->parameter = report_phys;
							new_trb->status    = (uint32_t)m_hi->mps;
							new_trb->control   = (1u << 10) | (1u << 5) | (m_hi->ring->pcs & 1u);

							if (++m_hi->ring->enq == LINK_INDEX) {
								m_hi->ring->trb[LINK_INDEX].control =
									(TRB_TYPE_LINK << 10) | (1u << 1) | (m_hi->ring->pcs & 1u);
								m_hi->ring->enq = 0;
								m_hi->ring->pcs ^= 1;
							}
							doorbell32[slot_from_event] = dci_from_event;
						}
					} else {
						// ── Transfer error on a keyboard slot → device disconnected ──
						uint32_t err_slot = slot_from_event;
						for (int k = 0; k < keyboard_count; k++) {
							if (keyboards[k]->slot_id == err_slot) {
								print((char*)"Transfer error on keyboard slot, code:");
								to_str(code, str); print(str);
								tsc_delay_ms(300);
								needsResetting = true;
								break;
							}
						}
						if (needsResetting) break;
					}
				}
			}
		}

		// 5. Immediate overlay update — cursor + ghost outline, no compositor.
		//    Throttle: only one repo-stamp after draining the entire USB event batch.
		if (event_count > 0) {
			wm_overlay_update(g_mouse_x, g_mouse_y, g_mouse_buttons);
		}

		// ── PORTSC disconnect polling ─────────────────────────────────────────
		//
		// Some xHCI controllers do not reliably generate a PSC event when a
		// device is unplugged from the same port it was originally enumerated on.
		// As a safety net we poll the PORTSC register directly: if CCS (bit 0)
		// is 0 on any keyboard's root port, the device is gone.
		//
		// IMPORTANT: only poll keyboards that are directly on a root port
		// (root_port_num != 0).  For hub-connected keyboards, port_num is the
		// hub's downstream port number, NOT a root port index.
		for (int k = 0; k < keyboard_count; k++) {
			uint8_t rp = keyboards[k]->root_port_num;
			if (rp == 0) continue;   // hub child — skip PORTSC poll
			volatile uint32_t* portsc = (volatile uint32_t*)(
				(uintptr_t)g_ops + 0x400 + (uint32_t)(rp - 1) * 0x10);
			if (!(*portsc & 1u)) {   // CCS = 0: no device on this port
				print((char*)"Keyboard disconnected (PORTSC poll), port:");
				to_str(rp, str); print(str);
				tsc_delay_ms(300);
				needsResetting = true;
				break;
			}
		}
		if (needsResetting) break;

		// ── Software key-repeat (mirrors Linux input_repeat_key()) ───────────
		//
		// Runs every iteration so repeat timing tracks wall-clock time via TSC,
		// not USB event arrival rate.
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

				if (key >= 0x4F && key <= 0x52) {
					if (shift && key == 0x52) term_scroll_view(-1);
					else if (shift && key == 0x51) term_scroll_view(1);
					else {
						tty_input('\x1b'); tty_input('[');
						char arrow_codes[] = {'C', 'D', 'B', 'A'};
						tty_input(arrow_codes[key - 0x4F]);
					}
				} else {
					char c = shift ? hid_to_ascii_shift[key] : hid_to_ascii[key];
					if (c) tty_input(c);
				}
			}
		}

		// --- PHASE 18.5: ATOMIC 60Hz COMPOSITOR ---
		// 60Hz scene compositor — runs ONLY when content changed (text, scroll,
		// drag committed). Cursor movement never reaches here anymore.
		static uint64_t last_compose_tsc = 0;
		static uint32_t boot_sync_frames = 30;

		if (g_needs_refresh || boot_sync_frames > 0 || true) {
			uint64_t now_refresh = rdtsc();
			if (now_refresh - last_compose_tsc > (tsc_hz / 60)) {
				last_compose_tsc = now_refresh;
				g_needs_refresh  = false;

				// Drive rainbow animation: 4 pixels per frame
				rain_scroll += 4;
				// Force refresh because the rainbow window is always moving
				bool animation_active = true; 

				__asm__ volatile("cli");
				uint32_t fx1 = g_dirty_min_x, fx2 = g_dirty_max_x;
				uint32_t fy1 = g_dirty_min_y, fy2 = g_dirty_max_y;
				g_dirty_min_y = fb->height; g_dirty_max_y = 0;
				g_dirty_min_x = fb->width;  g_dirty_max_x = 0;
				__asm__ volatile("sti");

				if (boot_sync_frames > 0) {
					fx1 = 0; fx2 = fb->width - 1;
					fy1 = 0; fy2 = fb->height - 1;
					boot_sync_frames--;
				}
				
				// If animating, only dirty the rainbow window region to save bandwidth.
				if (animation_active) {
					uint32_t rx1 = rain_x > 0 ? (uint32_t)rain_x : 0;
					uint32_t rx2 = (uint32_t)(rain_x + rain_w);
					int32_t  ry1_s = rain_y - 28;
					uint32_t ry1 = ry1_s > 0 ? (uint32_t)ry1_s : 0;
					uint32_t ry2 = (uint32_t)(rain_y + rain_h);
					
					if (rx1 < fx1) fx1 = rx1;
					if (rx2 > fx2) fx2 = rx2;
					if (ry1 < fy1) fy1 = ry1;
					if (ry2 > fy2) fy2 = ry2;
				}

				wm_compose_dirty(fx1, fx2, fy1, fy2);
				// wm_compose_dirty calls wm_overlay_reapply internally — no extra call needed

				term_dirty_reset();
			}
		}
	}

	// A disconnect or PSC event caused break — go back and re-enumerate
	// all ports.  Without this goto the kernel entry point returns into nothing.
	goto restart;
}