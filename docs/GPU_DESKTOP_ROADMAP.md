# GPU and display roadmap

Updated 2026-11-22. Supported Intel composition, page flips and RCS scaling
probe by default after owner validation. GPU_RENDER=0 selects blitter composition.
General platform priorities are in [NEXT_STEPS](NEXT_STEPS.md).

## Current renderer

Applications draw into off-screen surfaces and report damage. A roughly 60 Hz
compositor builds damaged regions in a master buffer and copies them into
Limine's scanout framebuffer using CPU/SSE stores on the fallback path.
Supported Intel hardware uses hidden scanout buffers and page flips by default.
Terminal, Boot Log and demos
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
commands and polled fences. The optional logical render engine is described
below. General GPU allocation, interrupt-driven submission and a client GPU API
remain pending. User-window backing is
contiguous physical memory mapped to userspace; system surfaces use kernel memory.

Recorded 800×600 laptop copies were CPU 296 µs, blitter 2,911 µs without cache
flush and 6,878 µs with `clflush`. These are historical measurements on that
machine, not current general performance claims. Validated composition and
page flipping now probe supported hardware by default.

## Experimental opaque composition

Build normally with `make iso` to probe supported hardware. `GPU=0` forces CPU
rendering, and `GPU_PRESENT=0` keeps GPU composition with CPU presentation.
The original presentation benchmark still selects a copy path for the explicit
developer configuration `GPU=1 GPU_COMPOSE=0`.

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
Composition and presentation now default to enabled with runtime checks.
The existing `Compositor` kernel task owns the WM;
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

### Experimental logical render context

`GPU_RENDER=1` adds a serial kernel-owned Gen9 RCS context using execlist
submission, private four-level PPGTT tables, a pinned context/ring/batch and
explicit UC render-surface cache policy. It implies the existing composition
and page-flip flags. A context fence and schedule-out must both complete before
CPU reuse. Gen9 VF invalidation and conservative Kaby Lake DC-flush sequencing
precede textured draws. Runtime RCS failure disables the hook and recomposes
the complete scene. Failed reset swaps the destination and pins GPU sources.

Boot tests target private RAM and compare textured scaling, clipped borders and
premultiplied source-over alpha against expected pixels before enabling the
hook. The first desktop integration handles opaque scaled window clients,
subtracting foreground windows and preserving CPU resize grips. Fonts, icons,
wallpaper and translucent client policy are additional integration work.
RCS uses the Gen9 force-noncoherent HDC context workarounds and explicit pixel
cache handoffs. The earlier no-flush diagnostic probe has been removed because
it does not establish a supported coherent mode under those required settings.
Source PPGTT mappings are reused only while page range and retirement epoch
match. Context/ring/batch/fence control-line flushing remains.

The fixed 3D state and SIMD16 sampler shader derive from MIT-licensed IGT v1.28
`rendercopy_gen9.c`; the license and pinned source URL accompany the template.
Host tests check packed state, clipping, alpha factors and context registers.
The QEMU backend exercises WM scaled-client pixels and partial-render fallback
without executing Intel commands. HUD includes RCS only after readback.

The first owner RCS log confirms logical-context fence and retirement succeeded.
The first texture probe produced pixels but failed at a nearest-sampler boundary:
expected FF4E1638, actual FF4E0B31 (the adjacent source row). The original
16-to-12 scale placed pixel centers exactly on texel edges. Readback now uses
1:1 copying and exact 2x enlargement with binary-exact, unambiguous coordinates,
keeping strict pixel comparisons and larger 32x32 border guards. Host regression
checks every probe sample stays at least a quarter texel from a selection edge.
The owner reported about 80% fullscreen demo CPU in the failed-probe build,
which retained CPU scaling. The subsequent physical log passes the context
fence and all textured copy, scaling and premultiplied alpha readbacks, followed
by the first vblank page-flip latch. Desktop correctness and CPU cost under
scaled workloads still need owner measurements.

The owner subsequently reported lag/high CPU confined to fullscreen win_test.
The performance revision adds retained maximized scaled-client damage, the
RCS coherence gate, source-map reuse and rectangle-sized flushes on the
noncoherent path. The demo now reports its changing top 32 source rows during
normal animation, expanding damage for paint/clear input. Its gradient/stamps
and animation rate are preserved. The same demo damage applies in CPU mode.
New performance and coherence behavior still need laptop measurements.

After that revision the owner reported improved speed, intermittent demo
tearing and a freeze within a few seconds, including a stopped pointer. RCS
submission now writes all four execlist descriptor words without reading the
write-only port and forces full context restore for serial requests. Portable
packet tests exercise 2,048 submissions across repeated ring wraps. These checks
do not establish that the physical freeze is resolved.

The demo now publishes immutable snapshots with a partial top-damage height,
retaining damage if publication defers. Publication and retirement stamping are
atomic against compositor-frame advancement. Existing publish callers keep
full damage when the optional height is zero. Laptop stability and tear removal
for this revision still require confirmation.

The owner confirmed tear removal but another freeze around eight seconds after
maximizing. The stability revision avoids submitting a wrapped RCS tail: after
127 serial packets, once the last fence and schedule-out have completed, it
resets only RCS and reinitializes its context/HWS/ring before reuse. Batch,
fence, page tables and pixel buffers retain their allocations. Failure enters
the existing reset/quarantine fallback. Boot Log records recycle start,
completed sequence and successful completion. A host test exercises 2,048
packets with this recycling policy and verifies that no wrapped tail is used.
Pixel clflush is restored conservatively while retaining partial repainting,
source-map reuse and immutable publication. Both ring reuse and cache sharing
remain hypotheses; physical sustained stability still needs confirmation.

Further owner feedback isolates failure to maximizing the fixed-canvas demo;
its unscaled small window uses BCS and does not exercise runtime RCS. Review
found missing non-MCR Gen9 context hang workarounds. Each RCS packet now emits
masked HDC_CHICKEN0 settings for non-coherent context save/restore and HDC
operation, plus CACHE_MODE_1 STC/partial-resolve disables, before invalidation.
These match i915 gen9_ctx_workarounds_init's documented TLB-invalidation/PSD-
flush hang precautions. Host tests verify register masks and ordering across
repeated packets. The settings address a real omission; whether they resolve
the laptop freeze is still physically unverified.

Owner comparison now confirms that resizing the same demo stays stable with
`GPU_RENDER=0`, while enabling RCS can freeze the whole machine too quickly to
save a crash log. The latest saved log contains two completed idle ring recycles
without a timeout. Review found the Kaby Lake `WaForGAMHang` PIPE_CONTROL
sequence was incorrectly emitted as null, DC flush, CS stall, invalidation.
It now matches i915 `gen8_emit_flush_rcs`: null, DC flush, invalidation/post-sync,
then CS stall. The trailing stall must follow invalidation. Sustained packet
tests check the complete sequence and scratch address. This is a command-order
correction, not physical confirmation that the freeze is resolved.

The owner now confirms fullscreen works after that correction, but certain
ordinary resized aspect ratios still freeze immediately. No exact dimensions
or failing packet are available yet. As containment, the scaled-scene planner
now accepts maximized clients only (the WM's existing `resize_grip=false`
snapshot). Ordinary scaled windows remain CPU-composited; native-size BCS and
GPU presentation stay enabled. Host shape coverage verifies that odd, narrow,
wide and mixed-scale ordinary clients do not skip CPU rendering or submit RCS.
QEMU checks an ordinary scaled fixture uses CPU and the maximized demo uses RCS.
This is a temporary restriction, not a fix for arbitrary-geometry RCS hangs.

The fullscreen stability inference was subsequently disproved: the owner saw a
freeze after several minutes. No geometry is established stable for sustained
RCS use. Reset review found `render_reset()` directly wrote GDRST without the
Gen8/9 `RING_RESET_CTL` request/ready handshake. i915 explicitly warns an
unprepared KBL reset can hang the system during batch execution. RCS now
requests reset preparation, waits with a deadline, resets only after the ack,
and cancels preparation on every exit. Failed preparation returns to existing
disable/quarantine handling without forcing the RCS domain reset. Portable
host tests run 2,050 acknowledged reset passes and exercise preparation and
domain timeout cleanup. This addresses another real protocol omission; it is
not confirmation of the physical freeze's cause or resolution.

The owner subsequently ran that reset-handshake revision for 800 seconds with
interaction and no freezes, reporting 5-7% CPU for maximized Rainbow or the demo
but about 30% with them side by side. The guarded baseline is preserved at
`6baa52b` on `codex/gpu-rcs-stable-800s`, with hash-verified USB/ISO/kernel copies
in `.storage-test/checkpoints/gpu-rcs-stable-800s-2026-11-20/`.

At the owner's request, `codex/gpu-resized-composition` restores ordinary
scaled-client RCS and retains the reset handshake. A new retained scene path
plans BCS and RCS work at the actual damage bounds. It copies uncovered desktop
gaps on CPU, preserves unchanged chrome, and draws exposed resize-grip marks
after GPU completion. Any uncovered window chrome or unsupported client falls
back to the full scene renderer. This avoids the old full-width CPU band for
side-by-side client animation. Damage crossing the visible desktop HUD is split
into a small complete top band and retained client work below it, with one
presentation and accumulated timings. Host/QEMU tests cover the planner, pixels,
occlusion, grips, undamaged pixels and failure recovery; physical restored-size
stability and measured CPU improvements remain unverified until a laptop run.

Physical feedback on the restored revision: boot `F:` about 15.9 ms, opening
the demo about 32.9 ms; ordinary enlargement freezes immediately while maximize
works. The freeze also occurs with every other window minimized. Other visible
BCS clients are therefore not required to trigger it. Those `F:` values measure
frame duration, not CPU utilization.

The next isolated change uses full-client rectangle vertices and exact 0/1 UVs,
with damage/occlusion applied through Gen9 RASTER scissor enable and an inclusive
SCISSOR_RECT. Previously, every damage fragment changed vertices and fractional
UV endpoints. Signed X/Y positions are packed independently and full vertices
must fit S16. The existing boot pixel/readback gate checks clipped copy, scaling,
premultiplied alpha and untouched borders before enabling desktop RCS. Host
tests decode fixed vertices and scissor, cover thin/one-pixel clips and negative
positions, and require all clip variants to preserve vertex bytes. The mock
renderer consumes the encoded geometry/scissor rather than the requested clip.
This is a controlled geometry/clipping change, not an established diagnosis or
physical confirmation that enlargement is safe.

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
