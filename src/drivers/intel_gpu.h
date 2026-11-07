#ifndef intel_gpu_h
#define intel_gpu_h

#include <stdint.h>
#include "structures.h"
#include "helpers.h"
#include "pagingstuff.h"
#include "heap.h"
#include "gpu_cmds.h"

// ─── Intel iGPU (Gen9 / Gen9.5 — Skylake/Kaby Lake, e.g. HD 620) ──────────
//
// Blitter-only 2D acceleration: solid fills and surface copies, with a CPU
// fallback in the compositor whenever the GPU path is unavailable or hangs.
//
// What the blitter can NOT do (Intel PRM, Blitter volume): alpha blending.
// "Alpha BLTs" are classified as 3D-engine operations. Anything with per-pixel
// alpha (translucent shadows, AA text over a background) stays on the CPU until
// someone writes a render-engine (RCS) path.
//
// All command encodings and register offsets live in gpu_cmds.h (no kernel
// dependencies). This file is the kernel glue:
// PCI discovery, MMIO/GGTT mapping, forcewake, reset, self-test, flip.

#define INTEL_VGA_VENDOR 0x8086

static const uint64_t IGPU_MMIO_VA_BASE = 0xFFFF'8800'0000'0000ULL;

// GGTT layout we use (all far above the firmware framebuffer near gfx 0):
static const uint64_t IGPU_RING_GFX_ADDR      = 0x04000000ULL; // 32 KB ring
static const uint64_t IGPU_FENCE_GFX_ADDR     = 0x04100000ULL; // 1 page, fence dword at +0
static const uint64_t BACKBUFFER_GFX_BASE     = 0x08000000ULL; // master backbuffer
static const uint32_t IGPU_RING_BYTES         = 32768;

struct IntelGPU {
    bool     found = false;
    uint16_t devid = 0;

    uint64_t ecam_virt_base = 0;
    uint8_t  ecam_start = 0, bus = 0, dev = 0, fn = 0;

    uint64_t mmio_phys = 0;   // BAR0 - GTTMMADR (registers + GTT)
    uint64_t mmio_size = 0;
    uint64_t mmio_virt = 0;

    uint64_t aperture_phys = 0; // BAR2 - GMADR
    uint64_t aperture_size = 0;
};

static IntelGPU g_igpu;
static uint64_t g_fb_gfx_addr = 0;          // graphics address of the framebuffer origin
static uint64_t g_backbuffer_gfx_addr = 0;  // graphics address of master backbuffer origin
static GpuRing  g_gpu;
static bool     g_gpu_ready = false;        // true only after the self-test passed
static uint32_t g_gpu_flips = 0, g_gpu_flip_failures = 0;
extern uint64_t tsc_hz;
extern char g_gpu_status[64];
static void gpu_status_set(const char* s) { char* d = g_gpu_status; while (*s && d < g_gpu_status + 62) *d++ = *s++; *d = 0; }
static void gpu_status_cat(const char* s) { char* d = g_gpu_status; while (*d) d++; while (*s && d < g_gpu_status + 62) *d++ = *s++; *d = 0; }
static void gpu_status_cat_num(uint64_t v) { char t[24]; to_str(v, t); gpu_status_cat(t); }
static bool g_gpu_skip_flush = false;      // true ONLY if the boot benchmark proved the GPU snoops CPU caches
static bool g_gpu_flip_preferred = false;  // true ONLY if the GPU path measured clearly faster than the CPU copy


// Checked by compositor_thread_main() in kernel.cpp.
volatile bool g_pause_compositor = false;

extern uint8_t* g_master_backbuffer;

static void gpu_log_hex(const char* label, uint64_t v) {
    char s[32]; print((char*)label); to_hex(v, s); print(s);
}

// ─── Pixel format helpers (unchanged) ──────────────────────────────────────
static void print_fb_pixel_format() {
    if (!fb) { print((char*)"print_fb_pixel_format: no framebuffer yet"); return; }
    char str[64];
    print((char*)"--- Framebuffer pixel format (from Limine) ---");
    print((char*)"bpp:"); to_str(fb->bpp, str); print(str);
    print((char*)"memory_model (1 = RGB):"); to_str(fb->memory_model, str); print(str);
    print((char*)"red_mask_size/shift:");
    to_str(fb->red_mask_size, str); print(str);
    to_str(fb->red_mask_shift, str); print(str);
    print((char*)"green_mask_size/shift:");
    to_str(fb->green_mask_size, str); print(str);
    to_str(fb->green_mask_shift, str); print(str);
    print((char*)"blue_mask_size/shift:");
    to_str(fb->blue_mask_size, str); print(str);
    to_str(fb->blue_mask_shift, str); print(str);
}

static inline uint32_t pack_pixel_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 0xFF) {
    if (!fb || fb->bpp != 32) return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;

    uint32_t red_mask   = ((fb->red_mask_size   >= 32) ? 0xFFFFFFFFu : ((1u << fb->red_mask_size)   - 1)) << fb->red_mask_shift;
    uint32_t green_mask = ((fb->green_mask_size >= 32) ? 0xFFFFFFFFu : ((1u << fb->green_mask_size) - 1)) << fb->green_mask_shift;
    uint32_t blue_mask  = ((fb->blue_mask_size  >= 32) ? 0xFFFFFFFFu : ((1u << fb->blue_mask_size)  - 1)) << fb->blue_mask_shift;

    uint32_t val = ((uint32_t)r << fb->red_mask_shift)   & red_mask;
    val         |= ((uint32_t)g << fb->green_mask_shift) & green_mask;
    val         |= ((uint32_t)b << fb->blue_mask_shift)  & blue_mask;

    uint32_t used_mask = red_mask | green_mask | blue_mask;
    uint32_t free_byte_lane = ~used_mask & 0xFF000000u;
    if (free_byte_lane == 0xFF000000u) val |= ((uint32_t)a << 24);
    return val;
}

// ─── Forcewake ──────────────────────────────────────────────────────────────
// On Gen9 Linux has three forcewake domains: RENDER (0xA278/ack 0xD84), MEDIA
// (0xA270/ack 0xD88) and GT (0xA188/ack 0x130044). The blitter registers at
// 0x22000 fall in Linux's gen9_fw_ranges under FORCEWAKE_GT - so this IS the
// right domain (Gen9 has no separate blitter forcewake domain).
#define FORCEWAKE_GT_GEN9      0xA188
#define FORCEWAKE_ACK_GT_GEN9  0x130044
#define FORCEWAKE_KERNEL_BIT   0x1

static bool gpu_forcewake_get() {
    volatile uint32_t* fw_req = (volatile uint32_t*)(g_igpu.mmio_virt + FORCEWAKE_GT_GEN9);
    volatile uint32_t* fw_ack = (volatile uint32_t*)(g_igpu.mmio_virt + FORCEWAKE_ACK_GT_GEN9);
    *fw_req = (FORCEWAKE_KERNEL_BIT << 16) | FORCEWAKE_KERNEL_BIT;
    uint32_t ack = 0;
    for (uint32_t i = 0; i < 1000000; i++) {
        ack = *fw_ack;
        if (ack & FORCEWAKE_KERNEL_BIT) break;
    }
    if (!(ack & FORCEWAKE_KERNEL_BIT)) { gpu_log_hex("GPU: forcewake never acked, ack=", ack); return false; }
    for (volatile uint32_t i = 0; i < 5000000; i++);   // settle after warm restart
    return true;
}

// Release: same masked-write form Linux uses (fw_clear = REG_MASKED_FIELD_DISABLE).
static bool g_gpu_fw_released = false;
static void gpu_forcewake_put() {
    volatile uint32_t* fw_req = (volatile uint32_t*)(g_igpu.mmio_virt + FORCEWAKE_GT_GEN9);
    volatile uint32_t* fw_ack = (volatile uint32_t*)(g_igpu.mmio_virt + FORCEWAKE_ACK_GT_GEN9);
    *fw_req = (FORCEWAKE_KERNEL_BIT << 16);
    for (uint32_t i = 0; i < 1000000; i++) if (!(*fw_ack & FORCEWAKE_KERNEL_BIT)) break;
    g_gpu_fw_released = true;
}

// ─── Engine reset ───────────────────────────────────────────────────────────
// GEN6_GDRST (0x941C), BLT domain = bit 3. Linux notes GDRST is outside the GT
// power well (no forcewake needed) and resets TWICE on pre-Gen12.7 hardware
// because the engine may still be mid-flight on the first pass. We do the same.
#define GEN6_GDRST      0x941C
#define GEN6_GRDOM_BLT  (1u << 3)

static bool gpu_engine_reset_once() {
    volatile uint32_t* gdrst = (volatile uint32_t*)(g_igpu.mmio_virt + GEN6_GDRST);
    *gdrst = GEN6_GRDOM_BLT;
    for (uint32_t i = 0; i < 1000000; i++) {
        if (!(*gdrst & GEN6_GRDOM_BLT)) return true;
    }
    return false;
}
static bool gpu_engine_reset() {
    for (int pass = 0; pass < 2; pass++)
        if (!gpu_engine_reset_once()) { print((char*)"GPU: blitter reset did not complete"); return false; }
    return true;
}

// ─── PCI discovery ──────────────────────────────────────────────────────────
// Sizing a BAR means writing 0xFFFFFFFF to it, during which the device claims
// garbage addresses. Caller must have decode disabled and interrupts off.
static void pci_read_bar64(uint64_t virt_base, uint8_t start, uint8_t bus, uint8_t dev, uint8_t fn,
                           uint8_t bar_off, uint64_t* out_addr, uint64_t* out_size) {
    uint32_t bar_lo = pci_cfg_read32(virt_base, start, bus, dev, fn, bar_off);
    uint8_t bar_type = (bar_lo >> 1) & 0x3;
    uint64_t addr;
    uint32_t size_probe_hi = 0xFFFFFFFF;

    pci_cfg_write32(virt_base, start, bus, dev, fn, bar_off, 0xFFFFFFFF);
    uint32_t size_probe_lo = pci_cfg_read32(virt_base, start, bus, dev, fn, bar_off);

    if (bar_type == 0x2) {
        uint32_t bar_hi = pci_cfg_read32(virt_base, start, bus, dev, fn, bar_off + 4);
        addr = ((uint64_t)bar_hi << 32) | (bar_lo & ~0xFULL);
        pci_cfg_write32(virt_base, start, bus, dev, fn, bar_off + 4, 0xFFFFFFFF);
        size_probe_hi = pci_cfg_read32(virt_base, start, bus, dev, fn, bar_off + 4);
        pci_cfg_write32(virt_base, start, bus, dev, fn, bar_off + 4, bar_hi);
    } else {
        addr = bar_lo & ~0xFULL;
    }
    pci_cfg_write32(virt_base, start, bus, dev, fn, bar_off, bar_lo);

    uint64_t size_mask = ((uint64_t)size_probe_hi << 32) | (size_probe_lo & ~0xFULL);
    *out_addr = addr;
    *out_size = (~size_mask) + 1ULL;
}

static void find_intel_gpu(uint64_t virt_base, uint8_t start, uint16_t bus_lo, uint16_t bus_hi) {
    for (uint16_t bus = bus_lo; bus <= bus_hi; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t id0 = pci_cfg_read32(virt_base, start, bus, dev, 0, 0x00);
            if (id0 == 0xFFFFFFFF) continue;

            uint8_t header_type = (pci_cfg_read32(virt_base, start, bus, dev, 0, 0x0C) >> 16) & 0xFF;
            int max_fn = (header_type & 0x80) ? 8 : 1;

            for (uint8_t fn = 0; fn < max_fn; fn++) {
                uint32_t id = pci_cfg_read32(virt_base, start, bus, dev, fn, 0x00);
                if (id == 0xFFFFFFFF) continue;

                uint16_t vendor = id & 0xFFFF;
                uint16_t devid  = (id >> 16) & 0xFFFF;
                uint32_t class_reg = pci_cfg_read32(virt_base, start, bus, dev, fn, 0x08);
                uint8_t class_code    = (class_reg >> 24) & 0xFF;
                uint8_t subclass_code = (class_reg >> 16) & 0xFF;

                if (vendor != INTEL_VGA_VENDOR || class_code != 0x03 || subclass_code != 0x00) continue;

                gpu_log_hex("Found Intel display controller, device id:", devid);
                if (!igpu_devid_supported(devid)) {
                    print((char*)"  -> not a Gen9/9.5 part this driver knows; leaving GPU path disabled");
                    continue;
                }

                // BAR sizing: decode OFF + interrupts OFF for the duration.
                uint64_t flags;
                __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
                uint16_t pcmd = pci_cfg_read16(virt_base, start, bus, dev, fn, 0x04);
                pci_cfg_write16(virt_base, start, bus, dev, fn, 0x04, pcmd & ~((1u << 0) | (1u << 1)));

                uint64_t m_phys, m_size, a_phys, a_size;
                pci_read_bar64(virt_base, start, bus, dev, fn, 0x10, &m_phys, &m_size);
                pci_read_bar64(virt_base, start, bus, dev, fn, 0x18, &a_phys, &a_size);

                pci_cfg_write16(virt_base, start, bus, dev, fn, 0x04, pcmd | (1u << 1) | (1u << 2));
                if (flags & (1u << 9)) __asm__ volatile("sti" ::: "memory");

                g_igpu.found = true;
                g_igpu.devid = devid;
                g_igpu.ecam_virt_base = virt_base;
                g_igpu.ecam_start = start;
                g_igpu.bus = bus; g_igpu.dev = dev; g_igpu.fn = fn;
                g_igpu.mmio_phys = m_phys;   g_igpu.mmio_size = m_size;
                g_igpu.aperture_phys = a_phys; g_igpu.aperture_size = a_size;

                gpu_log_hex("IGPU MMIO/GTT (GTTMMADR) phys:", m_phys);
                gpu_log_hex("IGPU MMIO/GTT size:", m_size);
                gpu_log_hex("IGPU aperture (GMADR) phys:", a_phys);
                gpu_log_hex("IGPU aperture size:", a_size);
                return;
            }
        }
    }
}

static void map_intel_gpu_mmio() {
    if (!g_igpu.found) return;
    g_igpu.mmio_virt = IGPU_MMIO_VA_BASE;
    map_mmio_region(g_igpu.mmio_phys, g_igpu.mmio_virt, g_igpu.mmio_size);
    print((char*)"Mapped IGPU MMIO/GTT region");
}

// ─── GGTT helpers ───────────────────────────────────────────────────────────
static volatile uint64_t* gtt_pte_ptr(uint64_t gfx_addr) {
    return (volatile uint64_t*)(g_igpu.mmio_virt + g_igpu.mmio_size / 2 + (gfx_addr / 4096ULL) * 8ULL);
}

// PTE format matches Linux gen8_ggtt_pte_encode(): phys | PRESENT.
static void gtt_insert_page(uint64_t gfx_addr, uint64_t phys) {
    *gtt_pte_ptr(gfx_addr) = (phys & ~0xFFFULL) | 0x1ULL;
}

// Linux writes GFX_FLSH_CNTL_GEN6 (0x101008, bit 0) after PTE updates on Gen9 big-core
// to flush the PTE write path and invalidate the GGTT TLB.
#define GFX_FLSH_CNTL_GEN6 0x101008
static void gtt_flush() {
    volatile uint32_t* f = (volatile uint32_t*)(g_igpu.mmio_virt + GFX_FLSH_CNTL_GEN6);
    *f = 1;
    (void)*f;
}

// Total GGTT size from GMCH_CTRL (PCI cfg 0x50, GGMS in bits 7:6; Linux
// gen8_get_total_gtt_size: 0 -> 0, n -> (1<<n) MB). Returns max valid gfx address.
static uint64_t gtt_max_gfx_address() {
    uint16_t ctl = pci_cfg_read16(g_igpu.ecam_virt_base, g_igpu.ecam_start,
                                  g_igpu.bus, g_igpu.dev, g_igpu.fn, 0x50);
    uint32_t ggms = (ctl >> 6) & 0x3;
    uint64_t gtt_bytes = ggms ? ((uint64_t)(1u << ggms) << 20) : 0;
    return (gtt_bytes / 8ULL) * 4096ULL;
}

static bool try_virt_to_phys(uint64_t va, uint64_t* out_phys) {
    volatile uint64_t* pml4 = (volatile uint64_t*)(HHDM + (read_cr3() & PTE_ADDR_MASK));
    uint64_t e4 = pml4[PML4_INDEX(va)];
    if (!(e4 & PRESENT)) return false;
    volatile uint64_t* pdpt = (volatile uint64_t*)(HHDM + (e4 & PTE_ADDR_MASK));
    uint64_t e3 = pdpt[PDPT_INDEX(va)];
    if (!(e3 & PRESENT)) return false;
    volatile uint64_t* pd = (volatile uint64_t*)(HHDM + (e3 & PTE_ADDR_MASK));
    uint64_t e2 = pd[PD_INDEX(va)];
    if (!(e2 & PRESENT)) return false;
    if (e2 & HUGE) { *out_phys = (e2 & PD_2M_ADDR_MASK) | (va & 0x1FFFFFULL); return true; }
    volatile uint64_t* pt = (volatile uint64_t*)(HHDM + (e2 & PTE_ADDR_MASK));
    uint64_t e1 = pt[PT_INDEX(va)];
    if (!(e1 & PRESENT)) return false;
    *out_phys = (e1 & PTE_ADDR_MASK) | (va & 0xFFFULL);
    return true;
}

// Maps [va, va+bytes) (page aligned) into the GGTT at gfx_base. All-or-nothing check.
static bool gtt_map_range(uint64_t gfx_base, uint8_t* va, uint64_t bytes) {
    if (((uint64_t)va & 0xFFF) || (gfx_base & 0xFFF)) return false;
    uint64_t pages = (bytes + 4095) / 4096;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys;
        if (!try_virt_to_phys((uint64_t)va + i * 4096, &phys)) return false;
        gtt_insert_page(gfx_base + i * 4096, phys);
    }
    return true;
}

// Page-aligned, zeroed kernel memory (kmalloc only guarantees 16 bytes).
static uint8_t* gpu_alloc_aligned(uint64_t bytes) {
    uint8_t* raw = (uint8_t*)kmalloc(bytes + 4096);
    if (!raw) return nullptr;
    uint8_t* p = (uint8_t*)(((uint64_t)raw + 4095) & ~4095ULL);
    memset(p, 0, bytes);
    return p;
}

// ─── Framebuffer GGTT check ─────────────────────────────────────────────────
static bool verify_framebuffer_gtt() {
    if (!fb || !fb->address) { print((char*)"GPU: no framebuffer"); return false; }
    uint64_t fb_phys = (uint64_t)fb->address - HHDM;
    gpu_log_hex("Framebuffer physical address:", fb_phys);

    if (fb_phys < g_igpu.aperture_phys || fb_phys >= g_igpu.aperture_phys + g_igpu.aperture_size) {
        print((char*)"GPU: framebuffer is NOT inside the GPU aperture - GPU path disabled");
        return false;
    }
    g_fb_gfx_addr = fb_phys - g_igpu.aperture_phys;
    gpu_log_hex("Framebuffer graphics address:", g_fb_gfx_addr);

    uint64_t pte0 = *gtt_pte_ptr(g_fb_gfx_addr);
    gpu_log_hex("Framebuffer GTT PTE 0:", pte0);
    if (!(pte0 & 1)) { print((char*)"GPU: framebuffer PTE not present - GPU path disabled"); return false; }
    return true;
}

// ─── Diagnostics ────────────────────────────────────────────────────────────
static void gpu_dump_state(const char* why) {
    volatile uint32_t* g = (volatile uint32_t*)(g_igpu.mmio_virt + GPU_BCS_BASE);
    print((char*)"--- BCS state ---"); print((char*)why);
    gpu_log_hex("HEAD:  ", g[GPU_RING_HEAD / 4]);
    gpu_log_hex("TAIL:  ", g[GPU_RING_TAIL / 4]);
    gpu_log_hex("CTL:   ", g[GPU_RING_CTL / 4]);
    gpu_log_hex("START: ", g[GPU_RING_START / 4]);
    gpu_log_hex("MI_MODE:", g[GPU_RING_MI_MODE / 4]);
    gpu_log_hex("IPEIR: ", g[GPU_RING_IPEIR / 4]);   // nonzero = command parser error
    gpu_log_hex("IPEHR: ", g[GPU_RING_IPEHR / 4]);   // the offending command header
    gpu_log_hex("ACTHD: ", g[GPU_RING_ACTHD / 4]);
    gpu_log_hex("fence: ", g_gpu.fence_cpu ? *g_gpu.fence_cpu : 0);
    gpu_log_hex("seqno: ", g_gpu.seqno);
}

// ─── Public operations ──────────────────────────────────────────────────────
// Copy a rect (x1,y1 inclusive .. x2,y2 exclusive) from the master backbuffer to
// the screen. Synchronous: returns only after the GPU has finished, so the
// compositor may immediately repaint the cursor / restore backbuffer pixels
// without racing an in-flight blit. Returns false on any problem; the caller
// must then do the copy on the CPU.
static bool gpu_flip_rect(uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2) {
    if (!g_gpu_ready || g_gpu.hung || g_gpu_fw_released) return false;
    if (x2 > fb->width)  x2 = fb->width;
    if (y2 > fb->height) y2 = fb->height;
    if (x1 >= x2 || y1 >= y2) return true;

    // GPU reads the backbuffer from RAM: push the CPU's dirty lines out first, unless the boot
    // benchmark verified the GPU snoops CPU caches (then only the fence is needed).
    if (!g_gpu_skip_flush) {
        for (uint32_t y = y1; y < y2; y++)
            gpu_clflush_nofence(g_master_backbuffer + (uint64_t)y * fb->pitch + x1 * 4, (x2 - x1) * 4);
    }
    gpu_mfence();

    uint32_t seq;
    bool ok = gpu_emit_copy(&g_gpu,
                            g_backbuffer_gfx_addr, fb->pitch, x1, y1,
                            g_fb_gfx_addr,         fb->pitch, x1, y1, x2 - x1, y2 - y1)
           && gpu_emit_fence(&g_gpu, &seq, true);
    if (ok) {
        gpu_ring_commit(&g_gpu);
        ok = gpu_fence_wait(&g_gpu, seq);
    }
    if (ok) g_gpu_flips++;
    else {
        g_gpu_flip_failures++;
        if (g_gpu.hung) { gpu_dump_state("flip timed out - GPU path disabled, CPU fallback active"); g_gpu_ready = false; }
    }
    return ok;
}

// Solid fill on the screen. Synchronous.
static bool gpu_fill_rect(uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2, uint32_t color) {
    if (!g_gpu_ready || g_gpu.hung || g_gpu_fw_released) return false;
    if (x2 > fb->width)  x2 = fb->width;
    if (y2 > fb->height) y2 = fb->height;
    if (x1 >= x2 || y1 >= y2) return true;
    uint32_t seq;
    bool ok = gpu_emit_fill(&g_gpu, g_fb_gfx_addr, fb->pitch, x1, y1, x2, y2, color)
           && gpu_emit_fence(&g_gpu, &seq, true);
    if (ok) { gpu_ring_commit(&g_gpu); ok = gpu_fence_wait(&g_gpu, seq); }
    if (!ok && g_gpu.hung) { gpu_dump_state("fill timed out"); g_gpu_ready = false; }
    return ok;
}

static inline uint32_t fb_read_px(uint32_t x, uint32_t y);
// ─── Boot benchmark: choose the fastest CORRECT copy path ─────────────────
static void gpu_log_dec(const char* label, uint64_t v) {
    char s[24]; print((char*)label); to_str(v, s); print(s);
}
static inline uint64_t gpu_ticks_to_us(uint64_t t) { return tsc_hz ? (t * 1000000ULL) / tsc_hz : t; }
static inline uint32_t bench_pat(uint32_t x, uint32_t iter) {
    return 0xFF000000u | ((x * 2654435761u + iter * 0x9E3779B1u) >> 8);
}
// Fill the top-left w x h of the backbuffer the way the rainbow does: one row, replicated.
static void bench_write_rect(uint32_t w, uint32_t h, uint32_t iter) {
    uint32_t* first = (uint32_t*)g_master_backbuffer;
    for (uint32_t x = 0; x < w; x++) first[x] = bench_pat(x, iter);
    for (uint32_t y = 1; y < h; y++) memcpy(g_master_backbuffer + (uint64_t)y * fb->pitch, first, (uint64_t)w * 4);
}
// Check pixels in the most recently written rows (the ones most likely still dirty in CPU cache).
static bool bench_verify(uint32_t w, uint32_t h, uint32_t iter) {
    uint32_t xs[4] = { 0, w / 3, (2 * w) / 3, w - 1 };
    uint32_t y0 = h > 48 ? h - 48 : 0;
    for (uint32_t y = y0; y < h; y += 2)
        for (int k = 0; k < 4; k++)
            if (fb_read_px(xs[k], y) != bench_pat(xs[k], iter)) return false;
    return true;
}

static void gpu_benchmark() {
    uint32_t w = fb->width  < 800 ? fb->width  : 800;  w &= ~15u;
    uint32_t h = fb->height < 600 ? fb->height : 600;
    const int WARM = 2, N = 6;
    uint64_t t_cpu = 0, t_gf = 0, t_gn = 0;
    bool gpu_ok = true, nf_ok = true;

    // A. CPU copy: exactly what the compositor's fallback loop does.
    for (int i = -WARM; i < N; i++) {
        bench_write_rect(w, h, 100 + i);
        uint64_t a = gpu_tsc();
        for (uint32_t y = 0; y < h; y++)
            memcpy_vram_sse_headless((uint8_t*)fb->address + (uint64_t)y * fb->pitch,
                                     g_master_backbuffer   + (uint64_t)y * fb->pitch, w * 4);
        __asm__ volatile("sfence" ::: "memory");
        uint64_t b = gpu_tsc();
        if (i >= 0) t_cpu += b - a;
    }
    // B. GPU copy with clflush of the dirty rows (always correct).
    g_gpu_skip_flush = false;
    for (int i = -WARM; i < N && gpu_ok; i++) {
        bench_write_rect(w, h, 200 + i);
        uint64_t a = gpu_tsc();
        bool ok = gpu_flip_rect(0, 0, w, h);
        uint64_t b = gpu_tsc();
        if (!ok || !bench_verify(w, h, 200 + i)) gpu_ok = false;
        if (i >= 0) t_gf += b - a;
    }
    // C. GPU copy with NO flush: only valid if the GPU snoops the CPU caches. Verified, not assumed.
    if (gpu_ok) {
        g_gpu_skip_flush = true;
        for (int i = -WARM; i < N && nf_ok; i++) {
            bench_write_rect(w, h, 300 + i);
            uint64_t a = gpu_tsc();
            bool ok = gpu_flip_rect(0, 0, w, h);
            uint64_t b = gpu_tsc();
            if (!ok || !bench_verify(w, h, 300 + i)) nf_ok = false;
            if (i >= 0) t_gn += b - a;
        }
        g_gpu_skip_flush = false;
    }

    gpu_log_dec("GPU bench (800x600 frame, microseconds) CPU copy:   ", gpu_ticks_to_us(t_cpu / N));
    if (!gpu_ok) { gpu_status_set("GPU:bad pixels->CPU"); print((char*)"GPU bench: GPU copy produced wrong pixels or failed - CPU path stays"); g_gpu_flip_preferred = false; return; }
    gpu_log_dec("GPU bench GPU copy + clflush:                       ", gpu_ticks_to_us(t_gf / N));
    if (nf_ok) gpu_log_dec("GPU bench GPU copy, no flush (verified coherent): ", gpu_ticks_to_us(t_gn / N));
    else       print((char*)"GPU bench: GPU does NOT snoop CPU cache (no-flush variant gave stale pixels) - clflush stays");

    uint64_t best_gpu = t_gf; bool use_noflush = false;
    if (nf_ok && t_gn < t_gf) { best_gpu = t_gn; use_noflush = true; }
    // Require a clear win (10%) before switching away from the proven CPU path.
    bool prefer = (best_gpu * 10 < t_cpu * 9);
    g_gpu_skip_flush = prefer && use_noflush;
    g_gpu_flip_preferred = prefer;
    gpu_status_set("cpu="); gpu_status_cat_num(gpu_ticks_to_us(t_cpu / N));
    gpu_status_cat("us gpu="); gpu_status_cat_num(gpu_ticks_to_us(best_gpu / N));
    gpu_status_cat(use_noflush ? "us(nof) " : "us ");
    gpu_status_cat(prefer ? "->GPU" : "->CPU");
    print(prefer ? (use_noflush ? (char*)"GPU bench: using GPU copy WITHOUT flush (faster, verified)"
                                : (char*)"GPU bench: using GPU copy with clflush (faster)")
                 : (char*)"GPU bench: CPU copy is as fast or faster - keeping CPU path (GPU stays initialised)");
}

// ─── Bring-up + self-test ───────────────────────────────────────────────────
static inline uint32_t fb_read_px(uint32_t x, uint32_t y) {
    volatile uint32_t* p = (volatile uint32_t*)((uint8_t*)fb->address + (uint64_t)y * fb->pitch) + x;
    gpu_clflush_nofence(p, 4); gpu_mfence();
    return *p;
}

// Returns true and sets g_gpu_ready only if everything works end to end.
// Never calls hcf(): any failure leaves the CPU path in charge.
static bool gpu_bringup_impl() {
    g_gpu_ready = false;
    if (!g_igpu.found || !g_igpu.mmio_virt) return false;
    if (!fb || fb->bpp != 32 || !g_master_backbuffer) { print((char*)"GPU: need 32bpp fb + master backbuffer"); return false; }

    if (!verify_framebuffer_gtt()) return false;

    // Sanity on geometry the PRM cares about.
    if (fb->pitch % 64 != 0 || fb->pitch > 32767) { print((char*)"GPU: fb pitch unsupported (need multiple of 64, <32768)"); return false; }
    if (g_fb_gfx_addr & 63) { print((char*)"GPU: fb gfx address not 64B aligned"); return false; }
    uint64_t fb_bytes = (uint64_t)fb->pitch * fb->height;
    if (g_fb_gfx_addr + fb_bytes > IGPU_RING_GFX_ADDR) { print((char*)"GPU: fb overlaps our GGTT scratch range"); return false; }
    if ((uint64_t)g_master_backbuffer & 4095) { print((char*)"GPU: master backbuffer must be page aligned (see kernel.cpp patch)"); return false; }

    uint64_t gtt_max = gtt_max_gfx_address();
    gpu_log_hex("GGTT addressable bytes:", gtt_max);
    if (BACKBUFFER_GFX_BASE + fb_bytes > gtt_max) { print((char*)"GPU: backbuffer would exceed GGTT size"); return false; }

    if (!gpu_forcewake_get()) return false;
    if (!gpu_engine_reset()) return false;

    uint8_t* ring_mem  = gpu_alloc_aligned(IGPU_RING_BYTES);
    uint8_t* fence_mem = gpu_alloc_aligned(4096);
    if (!ring_mem || !fence_mem) { print((char*)"GPU: out of memory for ring"); return false; }

    if (!gtt_map_range(IGPU_RING_GFX_ADDR,  ring_mem,  IGPU_RING_BYTES) ||
        !gtt_map_range(IGPU_FENCE_GFX_ADDR, fence_mem, 4096) ||
        !gtt_map_range(BACKBUFFER_GFX_BASE, g_master_backbuffer, fb_bytes)) {
        print((char*)"GPU: could not map ring/fence/backbuffer into GGTT");
        return false;
    }
    g_backbuffer_gfx_addr = BACKBUFFER_GFX_BASE;
    gtt_flush();

    g_gpu = GpuRing();
    g_gpu.cpu  = (volatile uint32_t*)ring_mem;
    g_gpu.size = IGPU_RING_BYTES;
    g_gpu.regs = (volatile uint32_t*)(g_igpu.mmio_virt + GPU_BCS_BASE);
    g_gpu.fence_cpu = (volatile uint32_t*)fence_mem;
    g_gpu.fence_gfx = IGPU_FENCE_GFX_ADDR;

    int rc = gpu_ring_hw_start(&g_gpu, (uint32_t)IGPU_RING_GFX_ADDR);
    if (rc) { gpu_log_hex("GPU: ring start failed at step ", rc); gpu_dump_state("ring start"); return false; }
    print((char*)"GPU: ring started");

    // ── Self-test (interrupts off: compositor must not touch the screen) ──
    g_pause_compositor = true;
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");

    g_gpu_ready = true;     // gpu_* ops refuse to run unless this is set
    int failures = 0;
    uint32_t red   = pack_pixel_color(0xFF, 0x00, 0x00);
    uint32_t green = pack_pixel_color(0x00, 0xFF, 0x00);
    uint32_t blue  = pack_pixel_color(0x00, 0x00, 0xFF);

    // T1: solid fill
    if (!gpu_fill_rect(100, 100, 200, 200, red) || fb_read_px(150, 150) != red || fb_read_px(100, 100) != red || fb_read_px(199, 199) != red) {
        print((char*)"GPU self-test T1 (fill) FAILED"); failures++;
    }
    // T2: screen -> screen copy (non-overlapping)
    if (g_gpu_ready) {
        uint32_t seq;
        bool ok = gpu_emit_copy(&g_gpu, g_fb_gfx_addr, fb->pitch, 100, 100,
                                        g_fb_gfx_addr, fb->pitch, 400, 400, 100, 100)
               && gpu_emit_fence(&g_gpu, &seq, true);
        if (ok) { gpu_ring_commit(&g_gpu); ok = gpu_fence_wait(&g_gpu, seq); }
        if (!ok || fb_read_px(450, 450) != red || fb_read_px(400, 400) != red || fb_read_px(499, 499) != red) {
            print((char*)"GPU self-test T2 (copy) FAILED"); failures++;
        }
    }
    // T3: backbuffer -> screen flip, repeated with alternating colours.
    // This is the exact path the compositor will use.
    int t3_bad = 0;
    for (int iter = 0; iter < 64 && g_gpu_ready; iter++) {
        uint32_t c = (iter & 1) ? green : blue;
        for (uint32_t y = 100; y < 200; y++) {
            uint32_t* row = (uint32_t*)(g_master_backbuffer + (uint64_t)y * fb->pitch) + 600;
            for (uint32_t x = 0; x < 100; x++) row[x] = c;
        }
        if (!gpu_flip_rect(600, 100, 700, 200) ||
            fb_read_px(600, 100) != c || fb_read_px(650, 150) != c || fb_read_px(699, 199) != c) t3_bad++;
    }
    if (t3_bad) { gpu_log_hex("GPU self-test T3 (flip x64) failures:", t3_bad); failures++; }

    if (failures || g_gpu.hung) {
        gpu_dump_state("self-test failed");
        g_gpu_ready = false;
    }
    if (g_gpu_ready) gpu_benchmark();
    // If the compositor will not use the GPU, do not keep it forced awake for nothing.
    if (g_gpu_ready && !g_gpu_flip_preferred) gpu_forcewake_put();
    if (flags & (1u << 9)) __asm__ volatile("sti" ::: "memory");
    g_pause_compositor = false;

    if (!g_gpu_ready)               print((char*)"GPU: self-test FAILED - staying on CPU path");
    else if (g_gpu_flip_preferred)  print((char*)"GPU: self-test PASSED - blitter flip enabled");
    else                            print((char*)"GPU: self-test PASSED - GPU verified; CPU copy is faster, compositor uses CPU path; GPU released");
    return g_gpu_ready;
}

static bool gpu_bringup() {
    gpu_status_set("GPU:init");
    bool ok = gpu_bringup_impl();
    if (!ok) gpu_status_set("GPU:init failed->CPU");
    return ok;
}

#endif
