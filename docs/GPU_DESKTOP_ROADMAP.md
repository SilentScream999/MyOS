# Roadmap: toward a Windows-like, GPU-composited desktop

*Last updated: 2026-11-07.* GPU/display technical claims retain the original sources (section 6); implementation status below reflects the current surface compositor.
See `docs/AI_HANDOFF.md` for constraints and conventions.
Clock, disk-backed desktop assets, persistent apps and installation priorities are
tracked in [NEXT_STEPS.md](NEXT_STEPS.md); wallpaper should be cached in RAM after
a VFS load, never fetched from disk during composition.

## 1. What the target is

A "Windows-like GPU desktop" (DWM-style) means, architecturally:

1. Every app draws into its **own off-screen surface** (with an alpha channel). Apps never touch the screen.
2. A **compositor** combines surfaces: z-order, transparency, shadows, effects, redrawing only what changed.
3. Composition runs on the **GPU** (surfaces become textures, windows become blended quads).
4. The finished frame is **presented by the display hardware flipping to a new buffer at vblank**, so there is no tearing.
5. A **GPU memory manager and command scheduler** back all of this, and apps can eventually use the GPU themselves.

Items 1, 2 and 4 are achievable without a 3D engine, and early compositors were CPU-based. The GPU (item 3) pays off for alpha
blending, scaling and effects; it does not pay off for the plain copies we do today (measured: the CPU copy is about 10x faster).

## 2. Where we are

| Capability | Status |
|---|---|
| Windows as surfaces + damage reporting | **Done** for Rainbow, Visual Test and user windows (`SYS_WIN_CREATE/SYNC/DAMAGE/DESTROY/GET_BUFFER`). Damage list of up to 8 rects (`src/damage.h`). |
| Terminal and Boot Log as surfaces | **Done (A1).** All windows use the generic surface blit path; the compositor's old special-case branches are removed. |
| Per-pixel alpha | **None.** Everything is opaque; the "glass" taskbar is a hand-built gradient. |
| Chrome and shadows as layers | **No.** Chrome is drawn by CPU code straight into one master backbuffer, back to front. No retained layers. |
| Surface pixel format | ARGB8888 by convention (`APIDocs/WindowAPI.md`); the compositor ignores the alpha byte. |
| GPU command submission | **Blitter ring only** (fill, copy), polled completion, no interrupts, no scheduling. Works on our Gen9 laptop. |
| GPU memory management | **GGTT only, fixed address constants.** No allocator. Window surfaces live in ordinary kernel heap memory, not GPU-visible memory. |
| GPU alpha blending / 3D | **Not started.** No render engine, 3D state or shaders. |
| Display control (page flip, planes, hardware cursor) | **Nothing.** Scanout is the bootloader's framebuffer and the compositor copies into it. |
| Apps using the GPU | **No path.** Only the kernel compositor can touch the GPU; there is no client API. |
| Hardware | Intel Gen9 only. QEMU cannot test any GPU code. |

## 3. Tearing and flicker: what is known

Observed symptoms: horizontal tearing during animation and window drag, cursor flicker, drag-outline flicker. **The cause has not been established.** Candidate explanations:

* **A. Classic tearing:** the CPU copies into the live scanout buffer while the display is reading it.
* **B. Self-inflicted flicker:** the cursor and the drag outline are erased and redrawn **directly on VRAM**, which flickers regardless of any vblank timing.
* **C. Copy longer than the blanking interval:** even a copy that starts at vblank tears if it takes longer than the vertical blanking window; only double-buffering fixes that.

**Measure before building anything for this.** Record a slow-motion video (240 fps phone) of a window drag and of the rainbow window, and note the HUD tail. A tear line that wanders points to A or C; a cursor that blinks points to B.

**What would fix tearing structurally** (the mechanism is confirmed in Linux's Intel driver, see section 6): double-buffered scanout. Render into an off-screen buffer, then point the display plane at it by writing the plane's surface-address register. On Skylake-era hardware plane registers are double-buffered, so the new address takes effect at the next vblank. The cursor has its own plane and base register and can be moved without touching the framebuffer at all, which addresses B. Whether this cures our particular tearing is a hypothesis until measured.

## 4. Staged plan

Each stage is independently shippable and testable, ordered by dependency and risk. Don't start a stage before the previous one boots cleanly on the laptop.

### Stage A: CPU compositor model (no hardware risk)
* **A1. Complete:** Terminal and Boot Log are surface-backed; the special-case compositor branches have been deleted. Earlier desktop behavior was verified on the test laptop; see the handoff for current build-specific verification.
* **A2.** Define the pixel format (premultiplied vs straight alpha; decide and document) and a per-window "opaque" flag. Opaque windows keep the fast copy; others use an SSE2 blend. Keep the blend in a pure header and check it against a scalar reference.
* **A3.** Retained layers: window decoration cached or composed separately; shadow as a pre-rendered nine-slice. Taskbar glass via real alpha instead of a baked gradient.
* **A4.** (optional) Client double-buffering: swap front/back on `WIN_SYNC` so a client never shows a half-drawn frame.

### Stage B: Display control (the tearing and cursor fix)
* **B0.** Measure (section 3). A read-only scanline logger is acceptable as a diagnostic; do not schedule frames from it.
* **B1.** **Hardware cursor plane.** Read how firmware configured the pipe and planes first; program the cursor plane; move it by writing its base/position registers. Remove the VRAM cursor erase/redraw. *Acceptance:* the cursor never flickers during a drag.
* **B2.** **Page flipping.** Allocate 2-3 scanout buffers, map them into the GGTT (the repo already maps ranges), compose into the back buffer, and flip by writing the primary plane's surface register outside the vblank-critical window. Keep everything else about the plane (format, stride, size) exactly as firmware set it; change only the surface address. Draw the drag outline inside the compositor, as a layer, instead of on VRAM.
  * Risks: a wrong alignment or unmapped address gives a blank or garbled screen; display power-saving features (panel self-refresh, framebuffer compression) may interact; watermarks matter if anything but the address changes.
  * **Fail safe:** read the register back after writing; on any mismatch restore the original surface address and fall back to the CPU copy. Gate it behind a build flag (for example `FLIP=1`), default off until verified.
  * *Acceptance:* no tear line in slow-motion video of window drag and the rainbow window.

### Stage C: GPU memory and synchronisation
A GGTT address-space allocator (replacing the fixed constants), pinning surfaces so the GPU can read them, interrupt-based fence completion (no more polling), and GPU hang detection and reset recovery.

### Stage D: Render engine (the long pole)
The first deliverable is modest: **blend one solid-colour quad into an off-screen surface and read it back**, then a textured quad, then compositor integration. Notes:

* Even a plain textured quad needs full 3D pipeline state, surface states and a **pixel-shader kernel**. Intel's own test suite carries a hand-assembled blit shader for Gen9.
* Open question to settle *before* committing: on Gen9, Linux submits everything through **logical ring contexts and execlists** and treats ring-buffer submission as the pre-Gen8 path. Our blitter works through the legacy ring on our hardware, but **that is an observation, not a guarantee, and it may not extend to the render engine.** Prototype with a trivial render-engine command first.
* Keep command and state packing in pure functions separate from kernel glue (as `gpu_cmds.h` does), so it can be checked off-target against the CPU blend within a tolerance.
* Keep a CPU fallback, and keep opt-in (`GPU=1`-style) until it is both faster and correct.

### Stage E: Client API and isolation
User-mode clients submit work through the kernel with validation, with per-process address spaces on the GPU. Only after apps and Window API v2 exist (see `docs/APP_LOADING.md`).

### Cross-cutting
* **Testing without rebooting the laptop:** QEMU cannot emulate this Intel GPU. A paravirtual GPU (virtio-gpu) might allow iterating on compositor and driver *structure*, but it would not exercise Intel-specific code, and its fit with this kernel has not been evaluated. Treat it as an idea, not a plan.
* **Every stage** gets a build flag, a fail-safe fallback to the previous path, and an update to this file and to `docs/AI_HANDOFF.md`.

## 5. Dependencies

* Stage A1 and the app work (`docs/APP_LOADING.md`) are independent and can interleave.
* Stage B needs nothing from the render engine.
* Stage D wants Stage A2 (alpha format decided) and Stage C (surfaces in GPU memory).
* Window API v2 should be designed knowing that surfaces will later become GPU-visible buffers.

## 6. Sources and confidence

"Verified" in this source table means the statement was found in the cited
source, not that the proposed display/render work has been hardware-tested.
The existing blitter and earlier surface compositor have separate runtime
verification recorded in the handoff.

| Claim | Status | Source |
|---|---|---|
| On Skylake-era display hardware, plane registers are double-buffered and armed by writing `PLANE_SURF` (or `CURBASE` for the cursor); Linux performs the writes inside a vblank-evasion window | **Verified** | https://lkml.iu.edu/hypermail/linux/kernel/1608.2/04656.html (i915 SKL watermark patch) |
| Gen8+ Intel GPUs submit work via logical ring contexts / execlists; ring-buffer submission is the older method | **Verified** | https://docs.kernel.org/gpu/i915.html |
| Linux's legacy ring-submission code asserts it is not used on execlist-capable hardware | **Verified** | https://codebrowser.dev/linux/linux/drivers/gpu/drm/i915/gt/intel_ring_submission.c.html |
| Intel's GPU test suite has a Gen9 render copy with a hand-written pixel-shader kernel (raw dwords) and Gen9 surface-state structures | **Verified** | https://patchwork.kernel.org/comment/22071911 ; `lib/gen9_render.h` in igt-gpu-tools |
| Our blitter-ring success on Gen9 does not imply render-engine ring submission works | **Reasoned** from the above; no source states it either way | n/a |
| The blitter cannot do general alpha blending | **Indirectly supported:** Mesa's old Intel driver falls back off the blitter whenever blending is more than a plain copy, and the comment in `intel_gpu.h` states it. The hardware-manual sentence has not been located. | https://git.replicant.us/replicant/external_mesa3d/tree/src/mesa/drivers/dri/i965/intel_pixel.c |
| Scanout buffers must be mapped in the GGTT, and the plane register takes a graphics address | **Understanding of the driver; not re-checked** (the repo already has a framebuffer-GGTT verification routine) | n/a |
| Alignment requirements for the plane surface address | **Not checked.** Look up in the display manual / `skl_universal_plane.c` before B2 | n/a |
| Panel self-refresh / framebuffer compression can interact with plane updates | **Not checked.** Listed as a risk only | n/a |
| Page flipping will cure our tearing | **Hypothesis.** The mechanism is verified; applicability depends on section 3's measurement | n/a |
| A CPU compositor is adequate for opaque windows and simple alpha | **Opinion**, supported by the 296 us measured copy | n/a |
| virtio-gpu is a useful test vehicle for this kernel | **Unverified idea** | n/a |

## 7. Measurements on record

800x600 frame copy on our laptop: CPU 296 us; GPU blitter 2,911 us (no cache flush) and 6,878 us (with `clflush`). The GPU path therefore stays off by default.
