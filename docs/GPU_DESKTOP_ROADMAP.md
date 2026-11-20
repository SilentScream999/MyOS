# GPU and display roadmap

Updated 2026-11-19. Keep Intel GPU bring-up opt-in and preserve the CPU path.
General platform priorities are in [NEXT_STEPS](NEXT_STEPS.md).

## Current renderer

Applications draw into off-screen surfaces and report damage. A roughly 60 Hz
compositor builds damaged regions in a master buffer and copies them into
Limine's scanout framebuffer using CPU/SSE stores by default. An experimental
`GPU_PRESENT=1` presenter uses hidden scanout buffers instead. Terminal, Boot Log and demos
use the same surface composition path as user windows. Damage lists hold up to
eight rectangles, with a full/global box used by some writers.

Rainbow requests animation damage only for exposed client regions and skips
surface rendering when fully covered or off-screen. CPU mode updates its full
backing surface when exposed. GPU mode uses a resident palette and phase instead
of rewriting that backing each frame; CPU-only rows are generated directly from
the same phase. Visibility subtraction relies on opaque client windows.

Fonts and PNG icons already use CPU alpha blending. Icon images use
premultiplied pixels; that does not change the client-window ABI. Window surface
alpha is currently ignored, and the taskbar's gradient is not transparency.
There are no retained decoration/shadow layers or client present acknowledgments.

The cursor is stamped into the master buffer before dirty-region presentation
and restored afterward; cursor/drag overlays also have an update path when the
scene is unchanged. Preserve those paths' ordering instead of assuming every
cursor draw is a direct post-flip VRAM write.
Pointer-only/ghost-outline frames bypass scene composition entirely. The
isolated overlay renderer is compiled with optimization independently of kernel
`OPT`, caches cursor rasterization, and skips unchanged overlay footprints.
Scene blits still reapply their overlay snapshots to preserve cursor/corner
ordering. The shell caret damages its two scanlines only on blink transitions,
and stops blinking when inactive, minimized, off-screen or covered.
Damage maxima are inclusive in both axes; surface endpoints and copy-loop ends
are exclusive. Preserve that conversion for every copied row, including the last
row of tiny caret damage, and include the last pixel when aligning presentations.

`GPU=1` brings up the Intel Gen9 blitter: PCI/MMIO, owned GGTT mappings, fill/copy
commands and polled fences. There is no general GPU allocator, render engine,
shader pipeline, interrupt-driven submission or client API. User-window backing is
contiguous physical memory mapped to userspace; system surfaces use kernel memory.

Recorded 800×600 laptop copies were CPU 296 µs, blitter 2,911 µs without cache
flush and 6,878 µs with `clflush`. These are historical measurements on that
machine, not current general performance claims. Default `GPU=0` stays in place.

## Experimental opaque composition

Build with `make clean && make iso GPU_COMPOSE=1` (implies `GPU=1`). Both flags
remain off by default. The original presentation benchmark still selects a copy
path for bring-up-only mode; composition is a separately requested experiment,
not a claim that the slower historical blitter copy became faster.

- The compositor snapshots clients in stacking order and subtracts all higher
  opaque window rectangles. Unsupported alignment/pitch, scaled clients and
  fragmented/over-budget plans retain CPU rendering. Bounded plans use static
  compositor-owned storage and preserve exclusive rectangle endpoints.
- The blitter copies supported visible clients into the RAM master buffer after
  CPU chrome and before menus, HUD and cursor. The bottom twelve client rows
  retain CPU rendering for resize grips. CPU/SSE still presents the finished
  scene and handles cursor/ghost overlays unless `GPU_PRESENT=1` is enabled.
  Runtime scene-composition batches never target scanout; the optional presenter
  submits a separate copy to a hidden scanout buffer. `GPU_COMPOSE=1` also skips the old onscreen boot blitter
  tests/benchmark; its pseudo-random 800x600 test pattern caused the reported
  colored square over the loading screen. `GPU=1 GPU_COMPOSE=0` retains those
  developer presentation diagnostics.
- Rainbow uploads its 720-pixel palette once. Each exposed rectangle gets one
  cyclic row, followed by non-overlapping GPU row-doubling copies with in-batch
  flush barriers. CPU full-surface animation generation stops in this mode.
- GGTT reservation prefers a non-present interval for ring, fence, master,
  palette and a bounded 64 MiB source mapping arena. Firmware can mark unused
  space present through shared scratch mappings; when no absent interval fits,
  the driver inspects each entry of long uniform alias runs. It requires at
  least 64 MiB of identical valid PTEs, excludes the framebuffer's physical
  pages, and reserves guard pages. It preserves the entire CPU aperture, at
  least the lower 512 MiB, a generous prefix beyond programmed/live active
  display-plane and cursor bases, and the top 32 MiB. Scratch reuse rejects
  unreadable, compressed, tiled, rotated or non-XRGB8888 display state. Failed
  bring-up restores displaced scratch PTEs only after a confirmed engine reset.
  Allocation diagnostics include requested bytes, the largest absent/alias
  interval, firmware control values and the selected range. Source bindings are
  reused while their page-aligned address/size list and retirement epoch match.
  The compositor invalidates the epoch before retired backing can be freed.
  Palette-only batches neither remap sources nor evict cached window bindings.
  This is not per-process GPU isolation or a general eviction allocator.
- Supported big-core Gen9 parts use private GPU PAT entry zero for GGTT cache
  policy. Boot selects write-back LLC caching while preserving the other seven
  entries, holding all three Gen9 forcewake domains and checking readback. It
  then runs sixteen offscreen CPU/GPU handoff iterations with deliberately dirty
  CPU source and destination cache lines, checking all copied pixels and nearby
  guards without pixel-cache flushing. Only a passing result enables runtime
  composition without full-region `clflush`. Failed policy/coherence checks keep
  CPU composition instead of using the known expensive flush-every-frame path.
  Ring/fence control-line flushing and ordering barriers remain. The compositor polls
  for 50 microseconds, then sleeps in 1 ms timer intervals, with a 200 ms deadline.
  Batches are drained before the ring can fill with unsubmitted commands.
- Boot verifies a normal surface copy and shifted/replicated rainbow with RAM
  readback before installing the composition hook. Runtime failure disables
  submission and resets the blitter. If reset fails, a preallocated replacement
  CPU master buffer quarantines the old GPU destination and pins retired client
  backing until reboot (intentional memory retention after failed reset). The WM
  recomposes the complete scene before resuming CPU presentation.

This is an opaque blitter compositor, not the planned render-engine/shader
desktop. Alpha blending, retained GPU decorations, GPU scaling, hardware cursor,
general modesetting, interrupt completion, idle forcewake management and client
GPU APIs remain future work. Same-mode page flipping is now a separate opt-in
experiment described below.

Host command/pixel tests and QEMU WM integration use separate test backends.
They check skipped full CPU rainbow generation, current animation pixels, and
partial-batch failure with destination quarantine and CPU recovery. They do not
execute Intel MMIO or validate GPU reset behavior. See [TESTING](TESTING.md).

The first physical `GPU_COMPOSE=1` attempt on device `0x5916` reached PCI/MMIO
discovery and framebuffer validation, then stopped at
`no unused GGTT interval for owned buffers` before starting the ring. This
exposed the original non-present-only allocation assumption. The firmware-alias
fallback has host regression coverage. The owner subsequently reported
`opaque+rainbow`, confirming composition enabled, but approximately 24% CPU with
Rainbow mostly covered versus 8% in CPU mode, about 6% when hidden, and up to
80% with it fully exposed. Those are owner observations, not controlled
benchmarks. They motivated the coherent-cache/binding-reuse changes above;
the owner subsequently confirmed about 8% mostly covered and 24% fullscreen,
versus roughly 60% fullscreen with CPU rendering.

## Experimental complete-frame presentation

`make clean && make iso GPU_PRESENT=1` implies composition and GPU bring-up.
Default flags remain zero. The existing `Compositor` kernel task owns the WM;
this does not create a separate userspace desktop executable.

The presenter copies the clean master with the GPU into one of two hidden
scanout buffers, renders small cursor/drag overlays there on the CPU, drains
stores and arms a synchronous SURF update. It waits for both the LIVE address
and a changed frame counter before reusing the previous front buffer. Scene
damage rectangles produce one presentation per logical frame; overlay-only
frames use the same path. This removes the CPU full-frame presentation copy
and targets the moving horizontal tear reported with fullscreen Rainbow.

Only an existing linear XRGB8888 firmware plane with exactly matching geometry,
stride and zero offsets is eligible. FBC, PSR, a hardware cursor and unknown
layouts retain CPU presentation. Timings, scalers and watermarks are preserved.
The display engine does not snoop CPU caches. Presentation therefore selects
uncached GPU PAT[0], requires the same dirty-cache CPU/GT coherence probe, and
uses dedicated PMM output pages whose sole CPU mappings are WC. If the UC probe
fails, bring-up retries the existing LLC composition policy and CPU presenter.

A failed flip restores the firmware framebuffer or directs CPU fallback to the
pinned live private scanout. Both private buffers remain pinned through rollback
so a late latch cannot read recycled RAM. GPU submission failures retain the
existing reset/quarantine behavior. HUD `GPU:opaque+rainbow flip UC` identifies
the new mode; `GPU: first vblank page flip latched` confirms the first live latch.

The retained fullscreen Rainbow path skips CPU background, window and shell
redraw only when an opaque maximized Rainbow owns every damaged pixel. Damage
over a title bar, taskbar, popup or intersecting foreground window uses the
ordinary scene planner. Higher windows outside that damage do not block it.

The owner's 1920x1080 `GPU_PRESENT=1` log on 0x5916 reached the display
probe but rejected SURF=0 / SURFLIVE=0x20. Plane selection now compares only
SURFLIVE address bits 31:12, matching runtime latch checks, and rejects all-ones
MMIO reads. A host regression preserves this readback case. The corrected
plane selection was subsequently tested by the owner: about 5% CPU with
fullscreen Rainbow and no visible tearing. This is a laptop observation,
not a controlled benchmark. The owner also suspects initialization fails after
warm restarts until the laptop sits or power is drained. A failed warm-boot log
identified a pre-ring GGTT allocation failure: only 0x33000 contiguous absent
bytes for a 0x57C5000 reservation, no uniform scratch candidate, and unusual
sampled PTE values. The relationship to warm restart remains an owner
observation. Failed allocation now logs host BGSM/BDSM, graphics control and
memory-map entries overlapping the GTT backing range. Those read-only diagnostics
are intended to distinguish stale firmware table contents from backing-memory
reservation problems before choosing a recovery strategy.

The follow-up failed log places BGSM=0xBD000000 in reserved memory-map type 1,
with BDSM=0xBD800000 and GGC=0x1C1. An additional boot-only fallback now claims
a dedicated non-display interval after a bounded full GT reset. It requires
successful display-prefix reconstruction, preserves the aperture/lower 512 MiB,
guard pages and top 32 MiB, saves every displaced PTE, then clears only its
owned interval. A failed bring-up restores those entries after a confirmed
engine reset; failed reset pins the mappings. The normal absent/scratch paths
still run first. Physical warm-restart recovery needs owner verification.

Host pixel/state tests and the mock QEMU presenter pass. They do not execute
Intel display registers or prove physical tearing removal, cache visibility or
performance. Broader laptop tests should include smoothness/frame rate alongside
CPU usage and repeated warm restarts. Font/icon rasterization, alpha and
scaling remain CPU work pending render-engine support.

## Remaining stages

1. **Finish the CPU surface contract.** Define alpha/opacity behavior for client
   windows separately from image alpha. Retain fast opaque copies, add tested
   blending, then cached decorations/shadows and optional client double buffering.
2. **Measure presentation problems.** Earlier hardware reports included tearing,
   cursor flicker and drag-outline flicker. Recheck the current build with slow-
   motion video; distinguish live-scanout copying from overlay redraw artifacts.
3. **Hardware cursor plane.** Read firmware pipe/plane state before changing it.
   Gate the new path, preserve rollback and verify on the laptop.
4. **Validate page flipping.** Physically verify the opt-in same-mode presenter,
   its cache visibility, LIVE/frame-counter synchronization and rollback. Wider
   formats, layouts and power-saving takeovers still need separate implementation.
   Waiting for vblank alone does not make a long live-framebuffer copy tear-free.
5. **GPU memory and synchronization.** Replace fixed GGTT offsets with allocation,
   pinning, fences, interrupts and hang/reset recovery.
6. **Render engine.** First blend one off-screen solid quad and read it back,
   then a textured quad, then compositor integration. Validate commands/state
   against CPU output before using it for the desktop.
7. **Client GPU API.** Add validated submissions and process isolation only after
   memory ownership and recovery are defined.

Display control does not depend on a render engine. Render-engine integration
does depend on a pixel contract and GPU-visible memory management. Successful
legacy blitter submission on this Gen9 laptop does not establish that the same
submission method is suitable for the render engine.

## Verification and references

QEMU does not emulate the target Intel GPU. Host command/state tests and a
possible future virtio-gpu driver cannot validate Gen9 MMIO/display behavior.
Each hardware stage needs an opt-in flag, bounded failures, a working fallback
and a recorded physical result. General testing is in [TESTING](TESTING.md).

Useful design references retained from the earlier investigation:

- [Linux i915 architecture](https://docs.kernel.org/gpu/i915.html): logical
  contexts, submission and driver structure.
- [i915 Gen8/9 private PAT setup](https://codebrowser.dev/linux/linux/drivers/gpu/drm/i915/gt/intel_gtt.c.html):
  write-back LLC policy for normal objects and entry-zero behavior for GGTT.
- [i915 GGTT scratch handling](https://codebrowser.dev/linux/linux/drivers/gpu/drm/i915/gt/intel_ggtt.c.html):
  unused mappings may reference a present scratch page; presence is not ownership.
- [Intel SKL plane registers](https://codebrowser.dev/linux/linux/drivers/gpu/drm/i915/display/skl_universal_plane_regs.h.html)
  and [cursor registers](https://codebrowser.dev/linux/linux/drivers/gpu/drm/i915/display/intel_cursor_regs.h.html):
  read-only reconstruction of active display bases before scratch reuse.
- [Intel ring-submission implementation](https://codebrowser.dev/linux/linux/drivers/gpu/drm/i915/gt/intel_ring_submission.c.html):
  legacy submission assumptions; recheck them before render-engine work.
- [Skylake plane-update discussion](https://lkml.iu.edu/hypermail/linux/kernel/1608.2/04656.html):
  plane updates, vblank evasion and watermark concerns.
- [Gen9 render-copy discussion](https://patchwork.kernel.org/comment/22071911):
  shader/state setup in igt-gpu-tools.

These are investigation starting points, not verification of our proposed code.
Plane-surface alignment, cache visibility, compression/self-refresh and current
submission requirements must be checked before implementing those stages.
