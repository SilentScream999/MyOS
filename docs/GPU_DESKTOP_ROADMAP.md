# GPU and display roadmap

Updated 2026-11-11. Keep Intel GPU bring-up opt-in and preserve the CPU path.
General platform priorities are in [NEXT_STEPS](NEXT_STEPS.md).

## Current renderer

Applications draw into off-screen surfaces and report damage. A roughly 60 Hz
compositor builds damaged regions in a master buffer and copies them into
Limine's scanout framebuffer using CPU/SSE stores. Terminal, Boot Log and demos
use the same surface composition path as user windows. Damage lists hold up to
eight rectangles, with a full/global box used by some writers.

Rainbow requests animation damage only for exposed client regions and skips
surface rendering when fully covered or off-screen. When exposed it updates its
complete backing surface at the same cadence, so uncovering reveals current
pixels. Visibility subtraction currently relies on opaque client windows.

Fonts and PNG icons already use CPU alpha blending. Icon images use
premultiplied pixels; that does not change the client-window ABI. Window surface
alpha is currently ignored, and the taskbar's gradient is not transparency.
There are no retained decoration/shadow layers or client present acknowledgments.

The cursor is stamped into the master buffer before dirty-region presentation
and restored afterward; cursor/drag overlays also have an update path when the
scene is unchanged. Preserve those paths' ordering instead of assuming every
cursor draw is a direct post-flip VRAM write.

`GPU=1` brings up the Intel Gen9 blitter: PCI/MMIO, fixed GGTT mappings, fill/copy
commands and polled fences. There is no GPU allocator, render engine, shader
pipeline, interrupt-driven submission or client API. User-window backing is
contiguous physical memory mapped to userspace; system surfaces use kernel memory.

Recorded 800×600 laptop copies were CPU 296 µs, blitter 2,911 µs without cache
flush and 6,878 µs with `clflush`. These are historical measurements on that
machine, not current general performance claims. Default `GPU=0` stays in place.

## Stages

1. **Finish the CPU surface contract.** Define alpha/opacity behavior for client
   windows separately from image alpha. Retain fast opaque copies, add tested
   blending, then cached decorations/shadows and optional client double buffering.
2. **Measure presentation problems.** Earlier hardware reports included tearing,
   cursor flicker and drag-outline flicker. Recheck the current build with slow-
   motion video; distinguish live-scanout copying from overlay redraw artifacts.
3. **Hardware cursor plane.** Read firmware pipe/plane state before changing it.
   Gate the new path, preserve rollback and verify on the laptop.
4. **Page flipping.** Compose into separate scanout buffers and arm the display
   plane at the appropriate time. Verify GGTT addresses, format/stride/alignment,
   synchronization and power-saving interactions from the hardware documentation.
   Do not assume waiting for vblank makes a long live-framebuffer copy tear-free.
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
- [Intel ring-submission implementation](https://codebrowser.dev/linux/linux/drivers/gpu/drm/i915/gt/intel_ring_submission.c.html):
  legacy submission assumptions; recheck them before render-engine work.
- [Skylake plane-update discussion](https://lkml.iu.edu/hypermail/linux/kernel/1608.2/04656.html):
  plane updates, vblank evasion and watermark concerns.
- [Gen9 render-copy discussion](https://patchwork.kernel.org/comment/22071911):
  shader/state setup in igt-gpu-tools.

These are investigation starting points, not verification of our proposed code.
Plane-surface alignment, cache visibility, compression/self-refresh and current
submission requirements must be checked before implementing those stages.
