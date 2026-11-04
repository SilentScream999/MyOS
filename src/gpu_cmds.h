#ifndef gpu_cmds_h
#define gpu_cmds_h

// ─── gpu_cmds.h ─────────────────────────────────────────────────────────────
// Portable (no kernel dependencies) command encoding + ring management for the
// Gen9 Blitter Command Streamer (BCS). Kept separate from intel_gpu.h so it has no kernel dependencies and builds on a PC.
//
// Every constant below was checked against Linux i915 (intel_gpu_commands.h,
// gen8_engine_cs.[ch], intel_engine_regs.h, intel_ring_submission.c) and Intel's
// Kaby Lake / Broadwell blitter PRM. See notes next to each item.

#include <stdint.h>
#include <stdbool.h>

#ifndef GPU_POLL_HOOK
#define GPU_POLL_HOOK() __asm__ volatile("pause" ::: "memory")
#endif
// ~100 ms at 3 GHz (up to ~300 ms at 1 GHz). Only used as a hang detector.
#ifndef GPU_TIMEOUT_TSC
#define GPU_TIMEOUT_TSC 300000000ULL
#endif

// ── Engine register offsets (bytes from the engine's MMIO base) ─────────────
// Linux: RING_TAIL/HEAD/START/CTL = base + 0x30/0x34/0x38/0x3c, MI_MODE = +0x9c,
// IPEIR/IPEHR/ACTHD = +0x64/+0x68/+0x74. BLT_RING_BASE = 0x22000.
#define GPU_BCS_BASE        0x22000u
#define GPU_RING_TAIL       0x30u
#define GPU_RING_HEAD       0x34u
#define GPU_RING_START      0x38u
#define GPU_RING_CTL        0x3Cu
#define GPU_RING_IPEIR      0x64u
#define GPU_RING_IPEHR      0x68u
#define GPU_RING_ACTHD      0x74u
#define GPU_RING_MI_MODE    0x9Cu
#define GPU_MI_MODE_STOP_RING (1u << 8)     // Linux: STOP_RING = REG_BIT(8)
#define GPU_HEAD_MASK       0x001FFFFCu     // Linux HEAD_ADDR
#define GPU_TAIL_MASK       0x001FFFF8u     // Linux TAIL_ADDR (qword aligned)
#define GPU_RING_VALID      1u

// ── Command encodings ───────────────────────────────────────────────────────
// MI_INSTR(opcode, flags) = (opcode << 23) | flags   (client = 0 = MI)
#define GPU_MI_INSTR(op, fl)        (((uint32_t)(op) << 23) | (uint32_t)(fl))
#define GPU_MI_NOOP                 0u
// NOTE: Linux's value is MI_INSTR(0x26,1) = 0x13000001 (0x26 << 23, NOT 0x26 << 24).
// 0x26000001 would decode as client=1, opcode=0xC - not a valid MI_FLUSH_DW.
#define GPU_MI_FLUSH_DW             GPU_MI_INSTR(0x26, 1)
#define GPU_MI_FLUSH_DW_OP_STOREDW  (1u << 14)
#define GPU_MI_FLUSH_DW_USE_GTT     (1u << 2)
#define GPU_MI_INVALIDATE_TLB       (1u << 18)

#define GPU_XY_COLOR_BLT_CMD        ((2u << 29) | (0x50u << 22))
#define GPU_XY_SRC_COPY_BLT_CMD     ((2u << 29) | (0x53u << 22))
#define GPU_BLT_WRITE_RGBA          (3u << 20)   // Linux BLT_WRITE_RGB|BLT_WRITE_A
#define GPU_BLT_DEPTH_32            (3u << 24)
#define GPU_BLT_ROP_SRC_COPY        (0xCCu << 16)
#define GPU_BLT_ROP_COLOR_COPY      (0xF0u << 16)

// ── Device-ID gate ──────────────────────────────────────────────────────────
// Register layout used here is Gen9/Gen9.5 (Skylake 0x19xx, Kaby Lake 0x59xx,
// Coffee/Whiskey Lake 0x3Exx, AML 0x87C0/0x87CA). IDs from Linux pciids.h.
// Deliberately NOT 0x5Axx (Apollo Lake: different forcewake/GGTT behaviour).
static inline bool igpu_devid_supported(uint16_t id) {
    uint8_t hi = (uint8_t)(id >> 8);
    if (hi == 0x19 || hi == 0x59 || hi == 0x3E) return true;
    return id == 0x87C0 || id == 0x87CA;
}

// ── Ring state ──────────────────────────────────────────────────────────────
struct GpuRing {
    volatile uint32_t* cpu = nullptr;   // CPU view of ring memory (size bytes, power of 2)
    uint32_t size = 0;
    uint32_t tail = 0;                  // software tail, bytes
    uint32_t flush_from = 0;            // bytes [flush_from, tail) not yet clflush'd/submitted
    volatile uint32_t* regs = nullptr;  // engine MMIO base as dword pointer
    volatile uint32_t* fence_cpu = nullptr; // CPU view of 8-byte-aligned fence dword
    uint64_t fence_gfx = 0;             // its GGTT address (8B aligned, bit 5 clear)
    uint32_t seqno = 0;
    bool ready = false;
    bool hung = false;
};

static inline uint64_t gpu_tsc() { return __builtin_ia32_rdtsc(); }

static inline void gpu_clflush_nofence(const volatile void* p, uint32_t len) {
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)63;
    uintptr_t e = (uintptr_t)p + len;
    for (; a < e; a += 64) __asm__ volatile("clflush (%0)" :: "r"(a) : "memory");
}
static inline void gpu_mfence() { __asm__ volatile("mfence" ::: "memory"); }

static inline uint32_t gpu_ring_head(const GpuRing* r) {
    return r->regs[GPU_RING_HEAD / 4] & GPU_HEAD_MASK;
}
// Free bytes ahead of tail, always leaving an 8-byte gap so head==tail means "empty".
static inline uint32_t gpu_ring_free(const GpuRing* r, uint32_t head) {
    return (head - r->tail - 8u) & (r->size - 1u);
}

// Starts the ring: mirrors Linux xcs_resume() (clear STOP_RING, program
// HEAD/TAIL to 0 with readback, START, CTL, verify RING_VALID sticks).
// Returns 0 on success, nonzero step number on failure (never hangs forever).
static int gpu_ring_hw_start(GpuRing* r, uint32_t ring_gfx) {
    volatile uint32_t* g = r->regs;
    uint64_t t0;

    g[GPU_RING_CTL / 4] = 0;
    // Masked-write register: high half = mask, low half = value. Clear STOP_RING.
    g[GPU_RING_MI_MODE / 4] = (GPU_MI_MODE_STOP_RING << 16);

    t0 = gpu_tsc();
    g[GPU_RING_HEAD / 4] = 0;
    while ((g[GPU_RING_HEAD / 4] & GPU_HEAD_MASK) != 0) {
        if (gpu_tsc() - t0 > GPU_TIMEOUT_TSC) return 1;   // HEAD won't clear
        g[GPU_RING_HEAD / 4] = 0;
    }
    g[GPU_RING_TAIL / 4] = 0;
    g[GPU_RING_START / 4] = ring_gfx;
    g[GPU_RING_CTL / 4] = (r->size - 4096u) | GPU_RING_VALID;

    t0 = gpu_tsc();
    while (!(g[GPU_RING_CTL / 4] & GPU_RING_VALID)) {
        if (gpu_tsc() - t0 > GPU_TIMEOUT_TSC) return 2;   // RING_VALID never latched
    }
    if (g[GPU_RING_START / 4] != ring_gfx) return 3;
    if ((g[GPU_RING_HEAD / 4] & GPU_HEAD_MASK) != 0 ||
        (g[GPU_RING_TAIL / 4] & GPU_TAIL_MASK) != 0) return 4;

    r->tail = 0; r->flush_from = 0; r->seqno = 0;
    r->ready = true; r->hung = false;
    return 0;
}

// Reserve `ndw` dwords (MUST be even so tail stays 8-byte aligned). If the
// command would straddle the end of the ring, pad the remainder with MI_NOOP and
// wrap first. Waits (bounded) for the engine to free space; on timeout marks the
// ring hung and returns false so callers can fall back to the CPU path.
static bool gpu_ring_reserve(GpuRing* r, uint32_t ndw) {
    if (!r->ready || r->hung || (ndw & 1u)) return false;
    uint32_t bytes = ndw * 4u;
    uint32_t need = bytes;
    bool wrap = (r->tail + bytes > r->size);
    if (wrap) need += (r->size - r->tail);
    if (need > r->size - 16u) return false;

    uint64_t t0 = gpu_tsc();
    while (gpu_ring_free(r, gpu_ring_head(r)) < need) {
        if (gpu_tsc() - t0 > GPU_TIMEOUT_TSC) { r->hung = true; return false; }
        GPU_POLL_HOOK();
    }
    if (wrap) {
        for (uint32_t o = r->tail / 4u; o < r->size / 4u; o++) r->cpu[o] = GPU_MI_NOOP;
        r->tail = 0;
    }
    return true;
}

static inline void gpu_emit(GpuRing* r, uint32_t dw) {
    r->cpu[r->tail / 4u] = dw;
    r->tail = (r->tail + 4u) & (r->size - 1u);   // exact-fit at ring end wraps to 0
}

// XY_COLOR_BLT, Gen8+ layout (7 dwords + 1 NOOP pad = 8). Rect is x1,y1 inclusive
// to x2,y2 exclusive. `pitch` is BYTES for a linear surface (PRM says "dwords";
// OSDev wiki and working hardware confirm bytes).
static bool gpu_emit_fill(GpuRing* r, uint64_t dst_gfx, uint32_t pitch,
                          uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2, uint32_t color) {
    if (!gpu_ring_reserve(r, 8)) return false;
    gpu_emit(r, GPU_XY_COLOR_BLT_CMD | GPU_BLT_WRITE_RGBA | 5u);
    gpu_emit(r, GPU_BLT_DEPTH_32 | GPU_BLT_ROP_COLOR_COPY | (pitch & 0xFFFFu));
    gpu_emit(r, (y1 << 16) | (x1 & 0xFFFFu));
    gpu_emit(r, (y2 << 16) | (x2 & 0xFFFFu));
    gpu_emit(r, (uint32_t)dst_gfx);
    gpu_emit(r, (uint32_t)(dst_gfx >> 32));
    gpu_emit(r, color);
    gpu_emit(r, GPU_MI_NOOP);
    return true;
}

// XY_SRC_COPY_BLT, Gen8+ layout (10 dwords). Dest rect (dx,dy,w,h), source origin (sx,sy).
static bool gpu_emit_copy(GpuRing* r,
                          uint64_t src_gfx, uint32_t src_pitch, uint32_t sx, uint32_t sy,
                          uint64_t dst_gfx, uint32_t dst_pitch, uint32_t dx, uint32_t dy,
                          uint32_t w, uint32_t h) {
    if (!gpu_ring_reserve(r, 10)) return false;
    gpu_emit(r, GPU_XY_SRC_COPY_BLT_CMD | GPU_BLT_WRITE_RGBA | 8u);
    gpu_emit(r, GPU_BLT_DEPTH_32 | GPU_BLT_ROP_SRC_COPY | (dst_pitch & 0xFFFFu));
    gpu_emit(r, (dy << 16) | (dx & 0xFFFFu));
    gpu_emit(r, ((dy + h) << 16) | ((dx + w) & 0xFFFFu));
    gpu_emit(r, (uint32_t)dst_gfx);
    gpu_emit(r, (uint32_t)(dst_gfx >> 32));
    gpu_emit(r, (sy << 16) | (sx & 0xFFFFu));
    gpu_emit(r, src_pitch & 0xFFFFu);
    gpu_emit(r, (uint32_t)src_gfx);
    gpu_emit(r, (uint32_t)(src_gfx >> 32));
    return true;
}

// Fence: Gen8+ MI_FLUSH_DW with a post-sync dword write. Identical shape to
// Linux __gen8_emit_flush_dw()/gen8_emit_ggtt_write(): 4 dwords =
// {(MI_FLUSH_DW+1)|OP_STOREDW, addr|USE_GTT, addr_hi, value}. Address must be 8B
// aligned with bit 5 clear (Linux w/a). The flush ahead of the store guarantees
// all prior blit writes have reached memory when the value becomes visible.
// Pass invalidate_tlb=true after changing GGTT PTEs.
static bool gpu_emit_fence(GpuRing* r, uint32_t* out_seq, bool invalidate_tlb) {
    if (!gpu_ring_reserve(r, 4)) return false;
    uint32_t seq = ++r->seqno;
    gpu_emit(r, (GPU_MI_FLUSH_DW + 1u) | GPU_MI_FLUSH_DW_OP_STOREDW |
                (invalidate_tlb ? GPU_MI_INVALIDATE_TLB : 0u));
    gpu_emit(r, (uint32_t)r->fence_gfx | GPU_MI_FLUSH_DW_USE_GTT);
    gpu_emit(r, (uint32_t)(r->fence_gfx >> 32));
    gpu_emit(r, seq);
    *out_seq = seq;
    return true;
}

// Make everything emitted since the last commit visible to the GPU and kick it.
static void gpu_ring_commit(GpuRing* r) {
    uint32_t from = r->flush_from, to = r->tail;
    if (to >= from) {
        gpu_clflush_nofence(r->cpu + from / 4u, to - from);
    } else {
        gpu_clflush_nofence(r->cpu + from / 4u, r->size - from);
        if (to) gpu_clflush_nofence(r->cpu, to);
    }
    gpu_mfence();   // also drains any pending WC writes to the framebuffer
    r->regs[GPU_RING_TAIL / 4] = r->tail & GPU_TAIL_MASK;
    (void)r->regs[GPU_RING_HEAD / 4];   // posting read
    r->flush_from = r->tail;
}

// Wait until the fence dword reaches `seq`. Bounded; on timeout marks the ring hung.
static bool gpu_fence_wait(GpuRing* r, uint32_t seq) {
    uint64_t t0 = gpu_tsc();
    for (;;) {
        gpu_clflush_nofence(r->fence_cpu, 4);
        gpu_mfence();
        uint32_t v = *r->fence_cpu;
        if ((int32_t)(v - seq) >= 0) return true;
        if (gpu_tsc() - t0 > GPU_TIMEOUT_TSC) { r->hung = true; return false; }
        GPU_POLL_HOOK();
    }
}

#endif
