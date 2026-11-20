#ifndef intel_gpu_h
#define intel_gpu_h

#include <stdint.h>
#include "structures.h"
#include "helpers.h"
#include "pagingstuff.h"
#include "heap.h"
#include "gpu_cmds.h"
#include "gpu_scene_commands.h"
#include "gpu_address_space.h"
#include "gpu_mapping_cache.h"
#include "gpu_display.h"
#include "gpu_presentation.h"

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

// GGTT addresses are selected from an unused interval above the firmware area.
static uint64_t IGPU_RING_GFX_ADDR, IGPU_FENCE_GFX_ADDR, BACKBUFFER_GFX_BASE;
static const uint32_t IGPU_RING_BYTES         = 32768;
static constexpr uint64_t GPU_SOURCE_BYTES = 64ULL*1024*1024;
static uint64_t g_gpu_source_base, g_gpu_palette_base;
static GpuGttSpan g_gpu_firmware_alias{};
static GpuGttSpan g_gpu_takeover{};
static uint64_t* g_gpu_takeover_ptes=nullptr;
static uint32_t* g_gpu_palette;
static uint8_t* g_gpu_recovery_buffer;
static uint64_t g_gpu_scene_batches=0,g_gpu_scene_pixels=0,g_gpu_scene_sleeps=0;
static bool g_gpu_compose_ready=false;
static bool g_gpu_scene_coherent=false;
static GpuMappingCache g_gpu_mapping_cache;
static uint64_t g_gpu_map_updates=0,g_gpu_map_reuses=0;
static uint64_t g_gpu_scanout_gfx[2]{};
static uint8_t* g_gpu_scanout_cpu[2]{};
static GpuDisplayPlane g_gpu_display_plane;
static GpuFlipState g_gpu_display_flip;
static uint8_t* g_gpu_firmware_scanout=nullptr;
static bool g_gpu_present_prepared=false,g_gpu_present_ready=false,g_gpu_scanout_uncached=false;
static uint64_t g_gpu_present_count=0,g_gpu_present_sleeps=0;
static void gpu_present_stop();
static bool gpu_prepare_display();

#ifndef GPU_COMPOSE
#define GPU_COMPOSE 0
#endif
#ifndef GPU_PRESENT
#define GPU_PRESENT 0
#endif

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

// Match i915's BDW/SKL private-PAT entry zero for GGTT: WB + LLC. The table is
// global GT state, so preserve its other seven entries and hold all three Gen9
// forcewake domains while touching it. Only the supported big-core Gen9 IDs
// reach this path; low-power Gen9 has a different snoop policy.
static bool gpu_setup_cache_policy(bool uncached=false) {
    const uint32_t requests[]={0xA278,0xA270},acks[]={0xD84,0xD88};
    unsigned requested=0;
    bool awake=true;
    for(unsigned i=0;i<2;++i){
        auto* req=(volatile uint32_t*)(g_igpu.mmio_virt+requests[i]);
        auto* ack=(volatile uint32_t*)(g_igpu.mmio_virt+acks[i]);
        *req=(FORCEWAKE_KERNEL_BIT<<16)|FORCEWAKE_KERNEL_BIT;++requested;
        uint64_t started=gpu_tsc(),timeout=tsc_hz?tsc_hz/20:GPU_TIMEOUT_TSC;
        while(!(*ack&FORCEWAKE_KERNEL_BIT)){
            if(gpu_tsc()-started>timeout){awake=false;break;}
            __asm__ volatile("pause");
        }
        if(!awake)break;
    }
    bool ok=false;
    if(awake){
        auto* pat=(volatile uint32_t*)(g_igpu.mmio_virt+0x40E0);
        uint32_t before=*pat;
        gpu_log_hex("GPU private PAT low before:",before);
        if(before!=UINT32_MAX){
            uint32_t desired=uncached?gpu_pat0_uncached(before):gpu_pat0_wb_llc(before);
            *pat=desired;uint32_t after=*pat;
            gpu_log_hex("GPU private PAT low after:",after);
            ok=after==desired;
            if(!ok){*pat=before;(void)*pat;}
        }
    }
    for(unsigned i=0;i<requested;++i){
        auto* req=(volatile uint32_t*)(g_igpu.mmio_virt+requests[i]);
        auto* ack=(volatile uint32_t*)(g_igpu.mmio_virt+acks[i]);
        *req=FORCEWAKE_KERNEL_BIT<<16;
        uint64_t started=gpu_tsc(),timeout=tsc_hz?tsc_hz/20:GPU_TIMEOUT_TSC;
        while(*ack&FORCEWAKE_KERNEL_BIT){
            if(gpu_tsc()-started>timeout)break;
            __asm__ volatile("pause");
        }
    }
    if(!ok)print((char*)"GPU: requested GGTT cache policy unavailable");
    return ok;
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

// Boot-only GT takeover. A full GT reset stops firmware render/media/blitter
// submissions before replacing non-display GGTT mappings. Display timings and
// scanout are outside this reset domain and retain their existing mappings.
static bool gpu_boot_gt_reset() {
    if(!gpu_forcewake_get())return false;
    auto* reset=(volatile uint32_t*)(g_igpu.mmio_virt+GEN6_GDRST);
    for(unsigned pass=0;pass<2;++pass){
        *reset=1u;(void)*reset;
        uint64_t started=gpu_tsc(),timeout=tsc_hz?tsc_hz/5:GPU_TIMEOUT_TSC;
        while(*reset&1u){
            if(gpu_tsc()-started>timeout){
                print((char*)"GPU: boot GT reset timed out; GGTT takeover cancelled");
                gpu_forcewake_put();return false;
            }
            __asm__ volatile("pause");
        }
    }
    return gpu_forcewake_get();
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
    if (e3 & HUGE) { *out_phys = (e3 & PDP_1G_ADDR_MASK) | (va & 0x3FFFFFFFULL); return true; }
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
    if (!bytes || gfx_base >= gtt_max_gfx_address() ||
        pages > (gtt_max_gfx_address()-gfx_base)/4096) return false;
    // Validate the complete CPU range before installing any PTEs.
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys;
        if (!try_virt_to_phys((uint64_t)va + i * 4096, &phys)) return false;
    }
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys;
        if (!try_virt_to_phys((uint64_t)va + i * 4096, &phys)) return false;
        gtt_insert_page(gfx_base + i * 4096, phys);
    }
    return true;
}

// A present PTE is not an allocation record: firmware may initialize unused
// GGTT space with repeated present mappings to one scratch page. Prefer absent
// entries, then consider only a fully inspected uniform scratch run beyond the
// aperture and a conservatively protected active-display prefix.
static bool gpu_reserve_gtt(uint64_t framebuffer_bytes) {
    uint64_t limit=gtt_max_gfx_address();
    uint64_t mmio_entries=(g_igpu.mmio_size/2)/8;
    if(limit>mmio_entries*4096)limit=mmio_entries*4096;
    if(limit>0x100000000ULL)limit=0x100000000ULL;
    uint64_t buffer_bytes=(framebuffer_bytes+4095)&~4095ULL;
    uint64_t bytes=IGPU_RING_BYTES+4096+buffer_bytes;
    if(GPU_COMPOSE)bytes+=4096+GPU_SOURCE_BYTES;
    if(GPU_PRESENT)bytes+=buffer_bytes*2;
    auto read=[](uint64_t address){return *gtt_pte_ptr(address);};
    auto free=gpu_gtt_largest_run(0x04000000,limit,read,
        [](uint64_t pte){return !(pte&1);},false);
    gpu_log_hex("GPU GGTT requested bytes:",bytes);
    gpu_log_hex("GPU GGTT largest non-present interval:",free.bytes);
    uint64_t base=free.bytes>=bytes?free.base:0;
    if(!base){
        uint64_t prefix=0;
        // Leave the top 32 MiB alone as well, including the GuC reservation
        // starting at 0xFEE00000 on a full 4 GiB GGTT.
        uint64_t end=limit>=0x02000000?limit-0x02000000:0;
        auto mmio=[](uint32_t offset){
            uint32_t value=*(volatile uint32_t*)(g_igpu.mmio_virt+offset);
            if((offset&0xFF)==0x80 && value){
                gpu_log_hex("GPU firmware display control register:",offset);
                gpu_log_hex("GPU firmware display control value:",value);
            }
            return value;
        };
        if(gpu_firmware_display_prefix(mmio,g_igpu.aperture_size,
                                       g_fb_gfx_addr+framebuffer_bytes,end,&prefix)){
            gpu_log_hex("GPU GGTT protected display prefix ends:",prefix);
            uint64_t physical_min=~0ULL,physical_max=0;
            uint64_t fb_end=(g_fb_gfx_addr+framebuffer_bytes+4095)&~4095ULL;
            for(uint64_t address=g_fb_gfx_addr&~4095ULL;address<fb_end;address+=4096){
                uint64_t physical=read(address)&0x0000FFFFFFFFF000ULL;
                if(physical<physical_min)physical_min=physical;
                if(physical>physical_max)physical_max=physical;
            }
            auto aliases=gpu_gtt_largest_run(prefix,end,read,
                [&](uint64_t pte){return gpu_firmware_scratch_pte(pte,physical_min,physical_max);},true);
            gpu_log_hex("GPU GGTT largest uniform scratch candidate:",aliases.bytes);
            // A few repeated pages can be an intentional live mapping. Require
            // at least 64 MiB of identical aliases, plus guard pages around ours.
            uint64_t needed=bytes+8192;
            if(needed<GPU_SOURCE_BYTES)needed=GPU_SOURCE_BYTES;
            if(aliases.bytes>=needed){
                base=aliases.base+4096;
                g_gpu_firmware_alias={base,bytes,aliases.pte};
                print((char*)"GPU: reserving verified uniform firmware scratch aliases");
                gpu_log_hex("GPU GGTT firmware scratch PTE:",aliases.pte);
            }
            if(!base){
                uint64_t candidate=gpu_takeover_base(prefix,end,bytes);
                uint64_t entries=bytes/4096;
                auto* saved=candidate?(uint64_t*)kmalloc(entries*sizeof(uint64_t)):nullptr;
                if(saved && gpu_boot_gt_reset()){
                    // Reset completed before the first mapping write. Preserve
                    // displaced entries for rollback if our bring-up fails.
                    for(uint64_t i=0;i<entries;++i)saved[i]=read(candidate+i*4096);
                    g_gpu_takeover={candidate,bytes,0};g_gpu_takeover_ptes=saved;
                    for(uint64_t i=0;i<entries;++i)*gtt_pte_ptr(candidate+i*4096)=0;
                    gtt_flush();base=candidate;
                    print((char*)"GPU: boot GT reset complete; reserved non-display GGTT interval");
                }else if(saved)kfree(saved);
            }
        }else print((char*)"GPU: cannot establish protected firmware display ranges");
    }
    if(base){
        IGPU_RING_GFX_ADDR=base;
        IGPU_FENCE_GFX_ADDR=IGPU_RING_GFX_ADDR+IGPU_RING_BYTES;
        BACKBUFFER_GFX_BASE=IGPU_FENCE_GFX_ADDR+4096;
        g_gpu_palette_base=BACKBUFFER_GFX_BASE+buffer_bytes;
        g_gpu_source_base=g_gpu_palette_base+4096;
        if(GPU_PRESENT){
            g_gpu_scanout_gfx[0]=g_gpu_source_base;g_gpu_scanout_gfx[1]=g_gpu_source_base+buffer_bytes;
            g_gpu_source_base+=buffer_bytes*2;
        }
        gpu_log_hex("GPU GGTT owned interval starts:",base);
        gpu_log_hex("GPU GGTT owned interval ends:",base+bytes);
        return true;
    }
    const uint64_t probes[]={0x04000000,0x08000000,0x10000000,0x20000000};
    for(uint64_t address:probes)if(address<limit){
        gpu_log_hex("GPU GGTT diagnostic graphics address:",address);
        gpu_log_hex("GPU GGTT diagnostic PTE:",read(address));
    }
    print((char*)"GPU: no unused GGTT interval for owned buffers");
    // Read-only warm-boot diagnostics. Do not infer ownership from arbitrary
    // present entries or overwrite mappings that cannot be reconstructed.
    uint32_t bgsm=pci_cfg_read32(g_igpu.ecam_virt_base,g_igpu.ecam_start,
                                 0,0,0,0xB4);
    gpu_log_hex("GPU host bridge BGSM raw:",bgsm);
    gpu_log_hex("GPU host bridge BDSM raw:",pci_cfg_read32(g_igpu.ecam_virt_base,
        g_igpu.ecam_start,0,0,0,0xB0));
    gpu_log_hex("GPU graphics control raw:",pci_cfg_read32(g_igpu.ecam_virt_base,
        g_igpu.ecam_start,g_igpu.bus,g_igpu.dev,g_igpu.fn,0x50));
    uint64_t table_base=bgsm&0xFFF00000u;
    uint64_t table_bytes=limit/4096*8;
    if(bgsm!=UINT32_MAX && table_base && memmap_req.response){
        for(uint64_t i=0;i<memmap_req.response->entry_count;++i){
            auto* entry=memmap_req.response->entries[i];
            if(entry->base<table_base+table_bytes && table_base<entry->base+entry->length){
                gpu_log_hex("GPU table overlapping memory-map base:",entry->base);
                gpu_log_hex("GPU table overlapping memory-map length:",entry->length);
                gpu_log_hex("GPU table overlapping memory-map type:",entry->type);
            }
        }
    }
    return false;
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

    uint64_t bytes=(uint64_t)fb->pitch*fb->height;
    uint64_t limit=gtt_max_gfx_address(),mapped_limit=(g_igpu.mmio_size/2)/8*4096;
    if(limit>mapped_limit)limit=mapped_limit;
    if(!bytes || g_fb_gfx_addr>=limit || bytes>limit-g_fb_gfx_addr ||
       bytes>g_igpu.aperture_size-g_fb_gfx_addr){
        print((char*)"GPU: framebuffer exceeds aperture/GGTT bounds");return false;
    }
    uint64_t end=(g_fb_gfx_addr+bytes+4095)&~4095ULL;
    for(uint64_t address=g_fb_gfx_addr&~4095ULL;address<end;address+=4096)
        if(!(*gtt_pte_ptr(address)&1)){
            print((char*)"GPU: incomplete firmware framebuffer mapping");return false;
        }

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
// Composition never submits writes to scanout. If reset fails, quarantine its
// old RAM destination permanently and rebuild the desktop in a preallocated CPU
// buffer. A late completion can then affect neither live pixels nor freed RAM.
static bool gpu_scene_abort(const char* reason) {
    gpu_present_stop();
    g_gpu_compose_ready=false;g_gpu_ready=false;
    g_gpu_compose_hook=nullptr;g_gpu_flip_hook=nullptr;
    gpu_dump_state(reason);
    bool stopped=gpu_engine_reset();
    if(!stopped)g_gpu_quarantine_sources=true;
    if(!stopped && g_gpu_recovery_buffer){
        g_master_backbuffer=g_gpu_recovery_buffer;
        g_gpu_recovery_buffer=nullptr;
    }
    gpu_status_set(stopped?"GPU:reset -> CPU":"GPU:quarantined -> CPU");
    // Keep the old GGTT mappings and owned allocations pinned until reboot.
    gpu_forcewake_put();
    return false;
}

// Poll briefly for fast batches, then block this compositor task. Timer wakeups
// work before GPU interrupts exist and avoid charging GPU execution to CPU busy
// time. Boot tests with IF clear keep their bounded polling behavior.
static bool gpu_scene_wait(uint32_t sequence) {
    uint64_t started=gpu_tsc(),spin=tsc_hz?tsc_hz/20000:150000;
    uint64_t timeout=tsc_hz?tsc_hz/5:GPU_TIMEOUT_TSC;
    uint64_t flags;__asm__ volatile("pushfq; pop %0":"=r"(flags));
    for(;;){
        gpu_clflush_nofence(g_gpu.fence_cpu,4);gpu_mfence();
        if((int32_t)(*g_gpu.fence_cpu-sequence)>=0)return true;
        uint64_t elapsed=gpu_tsc()-started;
        if(elapsed>timeout){g_gpu.hung=true;return false;}
        if(elapsed>spin && (flags&(1u<<9)) && g_current_task){
            ++g_gpu_scene_sleeps;task_sleep_ms(1);
        }else __asm__ volatile("pause");
    }
}

static bool gpu_scene_submit() {
    uint32_t sequence;
    if(!gpu_emit_fence(&g_gpu,&sequence,false))return false;
    gpu_ring_commit(&g_gpu);
    return gpu_scene_wait(sequence);
}

static bool gpu_compose_scene(const GpuScenePlan& plan) {
    if(!g_gpu_compose_ready || !g_gpu_ready || g_gpu.hung || g_gpu_fw_released)
        return false;
    if(plan.client_count>GpuScenePlan::max_clients || plan.count>GpuScenePlan::capacity)
        return gpu_scene_abort("GPU: invalid composition plan");
    uint32_t sources_used=0;
    for(unsigned i=0;i<plan.count;++i){
        if(plan.copies[i].source>=plan.client_count)
            return gpu_scene_abort("GPU: invalid composition source");
        sources_used|=uint32_t(1)<<plan.copies[i].source;
    }
    uint64_t sources[GpuScenePlan::max_clients]{};
    GpuMappingKey mapping_keys[GpuScenePlan::max_clients]{};
    unsigned mapping_count=0;
    uint64_t used=0;
    for(unsigned i=0;i<plan.client_count;++i){
        if(!(sources_used&(uint32_t(1)<<i)) || plan.clients[i].rainbow)continue;
        const auto& c=plan.clients[i];
        uint64_t address=(uint64_t)c.pixels,offset=address&4095;
        uint64_t bytes=((uint64_t)c.pitch*c.height+offset+4095)&~4095ULL;
        if(bytes>GPU_SOURCE_BYTES-used)
            return gpu_scene_abort("GPU: source mapping failed");
        mapping_keys[mapping_count++]={address-offset,bytes};
        sources[i]=g_gpu_source_base+used+offset;used+=bytes;
    }
    bool mappings_changed=!g_gpu_mapping_cache.matches(mapping_keys,mapping_count,g_gpu_surface_epoch);
    if(mappings_changed){
        uint64_t offset=0;
        for(unsigned i=0;i<mapping_count;++i){
            if(!gtt_map_range(g_gpu_source_base+offset,(uint8_t*)mapping_keys[i].address,mapping_keys[i].bytes))
                return gpu_scene_abort("GPU: source mapping failed");
            offset+=mapping_keys[i].bytes;
        }
        gtt_flush();
        g_gpu_mapping_cache.record(mapping_keys,mapping_count,g_gpu_surface_epoch);
        ++g_gpu_map_updates;
    }else ++g_gpu_map_reuses;
    // The software renderer uses non-temporal stores as well as cached stores.
    // Drain both before handing destination cache lines to the GPU.
    gpu_mfence();
    for(unsigned i=0;i<plan.count;++i){
        const auto& copy=plan.copies[i];const auto& r=copy.destination;
        if(copy.source>=plan.client_count || !visibility_valid(r) || r.x1<0 || r.y1<0 ||
           r.x2>(int)fb->width || r.y2>(int)fb->height)
            return gpu_scene_abort("GPU: invalid composition rectangle");
        const auto& c=plan.clients[copy.source];
        uint32_t width=r.x2-r.x1,height=r.y2-r.y1;
        if((uint64_t)copy.source_x+width>c.width || (uint64_t)copy.source_y+height>c.height)
            return gpu_scene_abort("GPU: source rectangle out of bounds");
        // Destination cache lines may include adjacent CPU-rendered chrome.
        // Write them back before GPU ownership, then invalidate after its fence.
        if(!g_gpu_scene_coherent){
            for(int y=r.y1;y<r.y2;++y)
                gpu_clflush_nofence(g_master_backbuffer+(uint64_t)y*fb->pitch+r.x1*4,width*4);
            if(!c.rainbow)for(uint32_t y=0;y<height;++y)
                gpu_clflush_nofence((uint8_t*)c.pixels+(uint64_t)(copy.source_y+y)*c.pitch+copy.source_x*4,width*4);
        }
    }
    gpu_mfence();
    uint32_t first_tail=g_gpu.tail;
    uint32_t mapping_sequence;
    if(mappings_changed && !gpu_emit_fence(&g_gpu,&mapping_sequence,true))
        return gpu_scene_abort("GPU: mapping invalidation rejected");
    for(unsigned i=0;i<plan.count;++i){
        // Leave enough room for the largest single rainbow operation and its
        // final fence. Never wait for ring space occupied by uncommitted work.
        uint32_t pending=(g_gpu.tail-first_tail)&(g_gpu.size-1);
        if(pending>g_gpu.size-4096){
            if(!gpu_scene_submit())return gpu_scene_abort("GPU: composition batch timed out");
            first_tail=g_gpu.tail;
        }
        const auto& copy=plan.copies[i];const auto& c=plan.clients[copy.source];
        const auto r=copy.destination;
        bool ok=c.rainbow ? gpu_emit_rainbow(&g_gpu,g_gpu_palette_base,
                            g_backbuffer_gfx_addr,fb->pitch,copy,plan.scroll)
            : gpu_emit_copy(&g_gpu,sources[copy.source],c.pitch,copy.source_x,copy.source_y,
                            g_backbuffer_gfx_addr,fb->pitch,r.x1,r.y1,r.x2-r.x1,r.y2-r.y1);
        if(!ok)return gpu_scene_abort("GPU: composition command rejected");
    }
    if(!gpu_scene_submit())return gpu_scene_abort("GPU: composition fence timed out");
    for(unsigned i=0;i<plan.count;++i){
        const auto r=plan.copies[i].destination;
        if(!g_gpu_scene_coherent)for(int y=r.y1;y<r.y2;++y)
            gpu_clflush_nofence(g_master_backbuffer+(uint64_t)y*fb->pitch+r.x1*4,(r.x2-r.x1)*4);
        g_gpu_scene_pixels+=(uint64_t)(r.x2-r.x1)*(r.y2-r.y1);
    }
    gpu_mfence();++g_gpu_scene_batches;
    return true;
}

static bool gpu_scene_initialize(const uint32_t* colors) {
    if(!colors)return false;
    uint64_t bytes=(uint64_t)fb->pitch*fb->height;
    g_gpu_recovery_buffer=gpu_alloc_aligned(bytes);
    g_gpu_palette=(uint32_t*)gpu_alloc_aligned(4096);
    if(!g_gpu_recovery_buffer || !g_gpu_palette ||
       !gtt_map_range(g_gpu_palette_base,(uint8_t*)g_gpu_palette,4096))return false;
    for(unsigned i=0;i<720;++i)g_gpu_palette[i]=colors[i];
    gpu_clflush_nofence(g_gpu_palette,2880);gpu_mfence();gtt_flush();
    // Verify GPU -> RAM visibility, shifted/replicated rainbow and a regular
    // surface copy before installing the hook. No scanout access is involved.
    static GpuScenePlan test{};
    test.client_count=2;test.count=2;test.accelerated=3;test.scroll=719;
    test.clients[0]={{},{},g_gpu_palette,720,1,2880,true,false};
    test.clients[1]={{},{},g_gpu_palette,720,80,2880,true,true};
    test.copies[0]={0,{0,0,64,1},17,0};
    test.copies[1]={1,{5,3,133,70},0,0};
    if(fb->width<133 || fb->height<70)return false;
    g_gpu_compose_ready=true;
    if(!gpu_compose_scene(test))return false;
    bool correct=true;
    for(unsigned x=0;x<64;++x)
        if(((uint32_t*)g_master_backbuffer)[x]!=colors[17+x])correct=false;
    for(unsigned y=3;y<70;++y){
        auto* row=(uint32_t*)(g_master_backbuffer+(uint64_t)y*fb->pitch);
        for(unsigned x=5;x<133;++x)if(row[x]!=colors[(719+x-5)%720])correct=false;
    }
    if(!correct)return gpu_scene_abort("GPU: offscreen composition readback FAILED");
    print((char*)"GPU: offscreen composition readback PASSED");

    // GGTT ignores per-PTE cache selection on Gen9 and uses private PAT[0].
    // Full-frame clflush on every handoff made the first version slower than
    // CPU rendering. Require WB/LLC policy AND dirty-cache readback checks before
    // enabling steady-state composition without those bulk cache operations.
    print((char*)(GPU_PRESENT ? "GPU: page flip requested by build" :
                                "GPU: page flip disabled by build (GPU_PRESENT=0)"));
    bool display_prepared=GPU_PRESENT && gpu_prepare_display();
    auto* source=(uint32_t*)gpu_alloc_aligned(256*128*4);
    if(!source){g_gpu_compose_ready=false;return false;}
    test.client_count=1;test.count=1;test.accelerated=1;
    test.clients[0]={{},{},source,256,128,1024,true,false};
    test.copies[0]={0,{5,3,133,67},17,19};
    auto pattern=[](unsigned x,unsigned y,unsigned iteration){
        return 0xFF000000u|((x*0x9E37u ^ y*0x7F4A3u ^ iteration*0x2468Bu)&0xFFFFFFu);
    };
    bool coherent=false;
    unsigned attempts=display_prepared?2:1;
    for(unsigned attempt=0;attempt<attempts && !coherent;++attempt){
        bool uncached=display_prepared && attempt==0;
        if(!gpu_setup_cache_policy(uncached))continue;
        gtt_flush();g_gpu_mapping_cache.valid=false;
        g_gpu_scene_coherent=true; // provisional: used only inside this boot test
        coherent=true;
        for(unsigned iteration=0;iteration<16 && coherent;++iteration){
        // Dirty both CPU source and destination caches on EVERY iteration.
        // Checking only a cold destination could miss stale CPU cache lines.
        for(unsigned y=0;y<128;++y)for(unsigned x=0;x<256;++x)
            source[y*256+x]=pattern(x,y,iteration);
        for(unsigned y=3;y<67;++y){
            auto* row=(uint32_t*)(g_master_backbuffer+(uint64_t)y*fb->pitch);
            for(unsigned x=4;x<=133;++x)row[x]=0xA5000000u|iteration;
        }
        if(!gpu_compose_scene(test))return false;
        for(unsigned y=3;y<67 && coherent;++y){
            auto* row=(volatile uint32_t*)(g_master_backbuffer+(uint64_t)y*fb->pitch);
            if(row[4]!=(0xA5000000u|iteration) || row[133]!=(0xA5000000u|iteration))coherent=false;
            for(unsigned x=5;x<133;++x)
                if(row[x]!=pattern(x-5+17,y-3+19,iteration)){coherent=false;break;}
        }
        }
        g_gpu_scanout_uncached=coherent && uncached;
        if(!coherent){
            g_gpu_scene_coherent=false;
            if(uncached)print((char*)"GPU: uncached scanout sharing failed; retrying LLC composition");
        }
    }
    if(!coherent){
        g_gpu_scene_coherent=false;g_gpu_compose_ready=false;
        gpu_status_set("GPU:coherence failed -> CPU");
        print((char*)"GPU: dirty CPU/GPU cache coherence FAILED - CPU composition retained");
        return false;
    }
    print((char*)"GPU: dirty CPU/GPU cache coherence PASSED - bulk clflush disabled");
    g_gpu_present_ready=display_prepared && g_gpu_scanout_uncached;
    gpu_status_set(g_gpu_present_ready?"GPU:opaque+rainbow flip UC":"GPU:opaque+rainbow LLC");
    return true;
}

// Scanout pages have exactly one CPU mapping: their HHDM mapping is converted
// to WC before first use, rather than adding a WC alias to a WB heap buffer.
// The display engine is not coherent with CPU caches. GT uses uncached writes
// for this mode, while a dirty-cache boot probe verifies CPU/GT snooping.
static uint8_t* gpu_alloc_scanout(uint64_t bytes) {
    uint64_t pages=(bytes+4095)/4096,physical=alloc_phys_pages(pages);
    if(!physical)return nullptr;
    auto* cpu=(uint8_t*)(HHDM+physical);
    gpu_clflush_nofence(cpu,pages*4096);gpu_mfence();
    TtyIrqGuard guard;
    for(uint64_t i=0;i<pages;++i)
        map_page((uint64_t)cpu+i*4096,physical+i*4096,WRITABLE|CACHE_DISABLE|NX);
    return cpu; // pinned until reboot, including failure/rollback
}

static bool gpu_display_wait(uint32_t expected,bool require_frame,uint32_t before) {
    uint64_t started=gpu_tsc(),timeout=tsc_hz?tsc_hz/5:GPU_TIMEOUT_TSC;
    uint64_t flags;__asm__ volatile("pushfq; pop %0":"=r"(flags));
    auto* live=(volatile uint32_t*)(g_igpu.mmio_virt+g_gpu_display_plane.live);
    auto* frames=(volatile uint32_t*)(g_igpu.mmio_virt+g_gpu_display_plane.frame_count);
    while(gpu_tsc()-started<timeout){
        if((*live&0xFFFFF000u)==expected && (!require_frame || *frames!=before))return true;
        if((flags&(1u<<9)) && g_current_task){++g_gpu_present_sleeps;task_sleep_ms(1);}
        else __asm__ volatile("pause");
    }
    return false;
}

static bool gpu_prepare_display() {
    auto read=[](uint32_t offset){return *(volatile uint32_t*)(g_igpu.mmio_virt+offset);};
    gpu_log_hex("GPU display framebuffer width:",fb->width);
    gpu_log_hex("GPU display framebuffer height:",fb->height);
    gpu_log_hex("GPU display framebuffer pitch:",fb->pitch);
    gpu_log_hex("GPU display FBC control:",read(0x43208));
    gpu_log_hex("GPU display eDP PSR1 control:",read(0x6F800));
    gpu_log_hex("GPU display eDP PSR2 control:",read(0x6F900));
    for(unsigned pipe=0;pipe<3;++pipe)for(unsigned plane=0;plane<3;++plane){
        uint32_t offset=pipe*0x1000+plane*0x100,control=read(0x70180+offset);
        if(control&(1u<<31)){
            gpu_log_hex("GPU display enabled plane register:",0x70180+offset);
            gpu_log_hex("GPU display enabled plane control:",control);
            gpu_log_hex("GPU display plane SURF:",read(0x7019C+offset));
            gpu_log_hex("GPU display plane LIVE:",read(0x701AC+offset));
            gpu_log_hex("GPU display plane STRIDE:",read(0x70188+offset));
            gpu_log_hex("GPU display plane SIZE:",read(0x70190+offset));
            gpu_log_hex("GPU display plane POS:",read(0x7018C+offset));
            gpu_log_hex("GPU display plane OFFSET:",read(0x701A4+offset));
            gpu_log_hex("GPU display pipe cursor control:",read(0x70080+pipe*0x1000));
        }
    }
    if(!gpu_find_display_plane(read,g_fb_gfx_addr,fb->width,fb->height,fb->pitch,&g_gpu_display_plane)){
        print((char*)"GPU: page flip unavailable (plane layout, cursor, FBC or PSR); CPU presentation retained");
        return false;
    }
    gpu_log_hex("GPU flip plane SURF register:",g_gpu_display_plane.surface);
    gpu_log_hex("GPU flip firmware surface:",g_gpu_display_plane.original_surface);
    // Confirm a running pipe without requiring a particular transcoder setup
    // (eDP can use the eDP transcoder with pipe A's frame counter).
    uint32_t before=read(g_gpu_display_plane.frame_count);
    if(!gpu_display_wait(g_gpu_display_plane.original_surface,true,before)){
        print((char*)"GPU: page flip unavailable (frame counter stopped)");return false;
    }
    uint32_t pat_low,pat_high;
    __asm__ volatile("rdmsr":"=a"(pat_low),"=d"(pat_high):"c"(0x277));
    if(((pat_low>>16)&255)!=1){print((char*)"GPU: page flip requires CPU PAT entry 2 = WC");return false;}
    uint64_t bytes=(uint64_t)fb->pitch*fb->height;
    for(unsigned i=0;i<2;++i){
        g_gpu_scanout_cpu[i]=gpu_alloc_scanout(bytes);
        if(!g_gpu_scanout_cpu[i] || !gtt_map_range(g_gpu_scanout_gfx[i],g_gpu_scanout_cpu[i],bytes)){
            print((char*)"GPU: page flip scanout allocation/mapping failed");return false;
        }
        g_gpu_display_flip.surfaces[i]=g_gpu_scanout_gfx[i];
    }
    gtt_flush();
    g_gpu_firmware_scanout=(uint8_t*)fb->address;
    g_gpu_present_prepared=true;
    print((char*)"GPU: double scanout buffers prepared; firmware plane unchanged until desktop presentation");
    return true;
}

static void gpu_display_arm(uint32_t surface,uint8_t* cpu) {
    // Arm only a complete hidden frame. Clear async/decrypt flags by using the
    // validated base alone. Update the panic framebuffer atomically with SURF,
    // so a fatal screen painted during a pending flip appears on that frame.
    TtyIrqGuard guard;
    auto* reg=(volatile uint32_t*)(g_igpu.mmio_virt+g_gpu_display_plane.surface);
    *reg=surface;(void)*reg;fb->address=cpu;
}

static void gpu_present_stop() {
    g_gpu_present_hook=nullptr;g_gpu_present_ready=false;
    if(!g_gpu_present_prepared || (g_gpu_display_flip.front<0 && g_gpu_display_flip.pending<0))return;
    // Rebuild the firmware buffer before returning to it. It is hidden while
    // our own surface is live; no old cursor/outline snapshots are carried over.
    if(g_gpu_firmware_scanout && g_master_backbuffer){
        memcpy_vram_sse_headless(g_gpu_firmware_scanout,g_master_backbuffer,(uint64_t)fb->pitch*fb->height);
        gpu_mfence();
        gpu_display_arm(g_gpu_display_plane.original_surface,g_gpu_firmware_scanout);
        if(!gpu_display_wait(g_gpu_display_plane.original_surface,false,0)){
            uint32_t live=*(volatile uint32_t*)(g_igpu.mmio_virt+g_gpu_display_plane.live)&0xFFFFF000u;
            for(unsigned i=0;i<2;++i)if(live==g_gpu_display_flip.surfaces[i]){
                // A failed rollback must still leave CPU rendering directed to
                // the actual live WC surface. Cancel a queued different flip.
                gpu_display_arm(live,g_gpu_scanout_cpu[i]);
                print((char*)"GPU: firmware flip rollback timed out; CPU uses pinned live scanout");
            }
        }
    }
    g_gpu_display_flip.pending=-1;
    // Both buffers and their GGTT mappings remain pinned; never recycle a page
    // that a late display latch could still read.
    g_dirty_min_x=0;g_dirty_min_y=0;g_dirty_max_x=fb->width-1;g_dirty_max_y=fb->height-1;
    g_needs_refresh=true;
}

static bool gpu_present_frame(const OverlayFrame& frame,const uint32_t* sprite) {
    if(!g_gpu_present_ready || !g_gpu_scanout_uncached || !g_gpu_ready || !g_desktop_ready)return false;
    int back=g_gpu_display_flip.writable();
    uint32_t ctl=*(volatile uint32_t*)(g_igpu.mmio_virt+g_gpu_display_plane.control);
    // No modesets or async flips while the borrowed firmware configuration is
    // in use. Power-saving state changes also require a different takeover.
    bool power_safe=true;
    const uint32_t power_regs[]={0x43208,0x6F800,0x6F900};
    for(uint32_t reg:power_regs){
        uint32_t value=*(volatile uint32_t*)(g_igpu.mmio_virt+reg);
        if(value==UINT32_MAX || (value&(1u<<31)))power_safe=false;
    }
    if(back<0 || ctl!=g_gpu_display_plane.control_value || !power_safe){
        gpu_present_stop();gpu_status_set("GPU:flip state changed -> CPU present");return false;
    }
    gpu_mfence();
    bool ok=gpu_emit_copy(&g_gpu,g_backbuffer_gfx_addr,fb->pitch,0,0,
                         g_gpu_scanout_gfx[back],fb->pitch,0,0,fb->width,fb->height)
            && gpu_scene_submit();
    if(!ok){gpu_scene_abort("GPU: hidden scanout copy failed");return false;}
    OverlayTarget target{g_gpu_scanout_cpu[back],g_master_backbuffer,(int)fb->width,(int)fb->height,(int)fb->pitch};
    const OverlayFrame empty{{-999,-999,0,0},{},0};
    overlay_render_transition(target,empty,frame,sprite,true);
    gpu_mfence(); // WC overlay stores must reach RAM before the display reads.
    uint32_t before=*(volatile uint32_t*)(g_igpu.mmio_virt+g_gpu_display_plane.frame_count);
    if(!g_gpu_display_flip.arm(back,before)){gpu_present_stop();return false;}
    gpu_display_arm(g_gpu_display_flip.surfaces[back],g_gpu_scanout_cpu[back]);
    if(!gpu_display_wait(g_gpu_display_flip.surfaces[back],true,before)){
        gpu_present_stop();gpu_status_set("GPU:flip timeout -> CPU present");return false;
    }
    uint32_t now=*(volatile uint32_t*)(g_igpu.mmio_virt+g_gpu_display_plane.frame_count);
    if(!g_gpu_display_flip.observe(g_gpu_display_flip.surfaces[back],now)){gpu_present_stop();return false;}
    if(!g_gpu_present_count)print((char*)"GPU: first vblank page flip latched");
    ++g_gpu_present_count;
    return true;
}

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
static bool gpu_bringup_impl(const uint32_t* colors) {
    g_gpu_ready = false;
    if (!g_igpu.found || !g_igpu.mmio_virt) return false;
    if (!fb || fb->bpp != 32 || !g_master_backbuffer) { print((char*)"GPU: need 32bpp fb + master backbuffer"); return false; }
    if (fb->width<700 || fb->height<500) { print((char*)"GPU: framebuffer too small for bring-up tests"); return false; }

    if (!verify_framebuffer_gtt()) return false;

    // Sanity on geometry the PRM cares about.
    if (fb->pitch % 64 != 0 || fb->pitch > 32767) { print((char*)"GPU: fb pitch unsupported (need multiple of 64, <32768)"); return false; }
    if (g_fb_gfx_addr & 63) { print((char*)"GPU: fb gfx address not 64B aligned"); return false; }
    uint64_t fb_bytes = (uint64_t)fb->pitch * fb->height;
    if ((uint64_t)g_master_backbuffer & 4095) { print((char*)"GPU: master backbuffer must be page aligned (see kernel.cpp patch)"); return false; }

    uint64_t gtt_max = gtt_max_gfx_address();
    gpu_log_hex("GGTT addressable bytes:", gtt_max);
    if (!gpu_reserve_gtt(fb_bytes)) return false;

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
    // Composition mode verifies GPU -> RAM and coherent sharing offscreen.
    // The old presentation benchmark writes random pixels into live scanout,
    // corrupting the loading screen until the desktop replaces it.
    if(!GPU_COMPOSE){
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
    }
    if (g_gpu_ready && GPU_COMPOSE && !gpu_scene_initialize(colors) && g_gpu_ready)
        gpu_status_set("GPU:composition unavailable->CPU");
    // If the compositor will not use the GPU, do not keep it forced awake for nothing.
    if (g_gpu_ready && !g_gpu_compose_ready && (GPU_COMPOSE || !g_gpu_flip_preferred)) gpu_forcewake_put();
    if (flags & (1u << 9)) __asm__ volatile("sti" ::: "memory");
    g_pause_compositor = false;

    if (!g_gpu_ready)               print((char*)"GPU: self-test FAILED - staying on CPU path");
    else if (g_gpu_present_ready)   print((char*)"GPU: self-test PASSED - GPU composition and page flip ready; awaiting first latch");
    else if (g_gpu_compose_ready)   print((char*)"GPU: self-test PASSED - opaque GPU composition enabled; CPU presentation");
    else if (!GPU_COMPOSE && g_gpu_flip_preferred) print((char*)"GPU: self-test PASSED - blitter flip enabled");
    else                            print((char*)"GPU: self-test PASSED - compositor uses CPU path; GPU released");
    return g_gpu_ready;
}

static bool gpu_bringup(const uint32_t* colors=nullptr) {
    gpu_status_set("GPU:init");
    bool ok = gpu_bringup_impl(colors);
    if(!ok && (g_gpu_firmware_alias.bytes || g_gpu_takeover.bytes) && !g_gpu_quarantine_sources){
        // Restore displaced firmware scratch entries only after a confirmed
        // reset. Never move mappings underneath an engine that may still DMA.
        if(gpu_engine_reset()){
            uint64_t end=g_gpu_firmware_alias.base+g_gpu_firmware_alias.bytes;
            for(uint64_t address=g_gpu_firmware_alias.base;address<end;address+=4096)
                *gtt_pte_ptr(address)=g_gpu_firmware_alias.pte;
            gtt_flush();g_gpu_firmware_alias={};
            for(uint64_t i=0;i<g_gpu_takeover.bytes/4096;++i)
                *gtt_pte_ptr(g_gpu_takeover.base+i*4096)=g_gpu_takeover_ptes[i];
            gtt_flush();g_gpu_takeover={};
            if(g_gpu_takeover_ptes){kfree(g_gpu_takeover_ptes);g_gpu_takeover_ptes=nullptr;}
            print((char*)"GPU: restored firmware scratch mappings after failed bring-up");
        }else g_gpu_quarantine_sources=true;
        if(!g_gpu_fw_released)gpu_forcewake_put();
    }
    if (!ok) gpu_status_set("GPU:init failed->CPU");
    return ok;
}

#endif
