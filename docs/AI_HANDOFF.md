# Project handoff

Updated 2026-11-20. This is the current working snapshot, not a change log.
Start with [README](../README.md), then the guide for the area being changed.
Pending work is in [NEXT_STEPS](NEXT_STEPS.md); commands are in [TESTING](TESTING.md).

## Current system

WyzarOS is a single-core x86-64 OS with a C++ kernel, C/C++ userspace, Limine boot,
preemptive tasks, static ELF loading and a framebuffer desktop. The long-term
target is a GPU-composited desktop with installable applications.

| Area | Implemented | Details |
|---|---|---|
| Boot | ISO/UEFI USB; framebuffer mapped before drawing; RAM recovery | [Storage](PERSISTENT_STORAGE.md) |
| Processes | Private kernel stacks/address spaces, spawn, exact-child wait, foreground cancellation | [Apps](APP_LOADING.md) |
| Desktop | Surface-backed windows, damage tracking, taskbar, Start, CPU presentation | [Display roadmap](GPU_DESKTOP_ROADMAP.md) |
| App resources | Portable `.wiz` packages, verified install/pin/remove, metadata/icons, `/apps` and `/bin` discovery | [Packages](APP_PACKAGES.md) |
| File Explorer | Native userspace browser, file operations, text previews and app launching | [Explorer](EXPLORER.md) |
| Notepad | Native text editing, atomic saving, unsaved prompts and Explorer Open with | [Notepad](NOTEPAD.md) |
| Task Manager | Core 0 CPU history, grouped programs/background/kernel tasks and opt-in Kernel / Input breakdown (ABI 10) | [Syscalls](../APIDocs/Syscalls.md) |
| Images | PNG/JPEG wallpapers; lazy PNG icons, alpha, themes and bounded caching | [Images](IMAGES.md) |
| Image Viewer | Native PNG/JPEG app, Explorer association, fit/zoom/pan, bounded userspace arena | [Viewer](IMAGE_VIEWER.md) |
| Fonts | Runtime TTF selection with built-in fallback; printable ASCII UI | [Fonts](FONTS.md) |
| Storage | Journaled ext4 USB root, persistent apps/settings/files, atomic saves, automatic capacity setup | [ext4](EXT4.md) |
| Input | xHCI HID, USB hubs, descriptor-based pointers, PS/2 keyboard/Synaptics | [USB/input](USB_INPUT.md) |
| GPU | Intel Gen9 bring-up; experimental opaque composition and resident rainbow palette; opt-in | [Display roadmap](GPU_DESKTOP_ROADMAP.md) |

Normal new images contain a 256 MiB ext4 seed and occupy 385 MiB overall.
First boot can provision a larger root on a larger stick. This disk seed reserve
is separate from the recovery archive in RAM.

After the disk shell reaches its first stdin request, unused recovery RAMFS
nodes and archive/fallback-module pages are reclaimed. `/tmp`, `/run` and live
logs remain. Failed disk selection or shell loading retains recovery. Removing
the disk later offlines storage until reboot; saves never switch silently to RAM.

## Decisions and implementation constraints

- The owner ran the RCS reset-handshake revision for 800 seconds without
  freezes while interacting, including maximizing and restoring. That version
  still guarded ordinary scaled clients onto CPU. Preserve `6baa52b` on
  `codex/gpu-rcs-stable-800s`; hash-verified USB/ISO/kernel copies are under
  `.storage-test/checkpoints/gpu-rcs-stable-800s-2026-11-20/`.
  The owner explicitly authorized restoring ordinary resized-client RCS and
  improving side-by-side composition. Work is on `codex/gpu-resized-composition`.
  Retained damage now combines BCS/RCS client copies, CPU wallpaper gaps and
  small exposed CPU grips; chrome/unsupported clients force full CPU repaint.
  The restored geometry coverage and physical CPU savings still need a laptop
  run. Retain the reset handshake and KBL null/DC/invalidate/trailing-stall order.

- `GPU=1` is now the default after owner validation. Composition follows GPU,
  presentation follows composition, and runtime discovery/readback select the
  supported hardware path or CPU fallback. `GPU=0` forces CPU mode. Keep
  `GPU_RENDER=0` and kernel `OPT=-O0` as defaults. The image codec and isolated
  overlay pixel renderer have their own optimized rules. Test kernel-wide
  optimization changes separately.
- QEMU does not emulate our Intel Gen9 GPU. Keep CPU fallbacks and bounded
  hardware waits; record what was actually tested on physical hardware.
- Apps are files plus metadata. Do not add new applications permanently to
  `wm_init`. Boot Log, Rainbow and Visual Test remain built-in fixtures.
  All terminals use the userspace console host with independent channels.
- Separate parser/state logic from MMIO so host tests can exercise actual code.
- Single-core IRQ guards are not SMP locks. Storage uses a sleeping mutex plus
  short IRQ guards; do not restore interrupt-disabled runtime disk transactions.
- TTY reads block until input, a queued shell launch or cancellation wakes them;
  do not replace them with runnable yield loops. CPU idle intervals end at
  interrupt entry, before a handler can switch tasks, to exclude scheduled work.
- Renderers must not read files or decode images. Resource work publishes
  complete snapshots only after input/compositor readers finish.
- Surface rectangles have exclusive right/bottom edges; the dirty-region
  presenter takes inclusive bounds. Preserve conversions and cursor ordering.
- Rainbow animation damages exposed client regions only and skips rendering
  when fully covered/off-screen. CPU mode keeps a complete backing surface when
  exposed. GPU mode uses an immutable palette plus phase and generates CPU-only
  rows directly; do not read its stale full backing as the current image. The
  visibility calculation assumes today's opaque client windows.
- GPU composition completes before CPU menus/HUD/cursor. CPU presentation is the
  fallback; supported GPU presentation copies the clean master into a hidden GPU
  scanout, overlays its cursor/outline, then arms a synchronous plane flip.
  Never reuse/free a batch's sources before its fence/reset. Failed reset pins
  the old destination and swaps to a reserved CPU buffer before full recomposition.
- Cached GPU source bindings require both matching virtual page ranges and the
  surface retirement epoch. Bump that epoch before the collector frees backing.
  Coherent composition requires private PAT[0] policy setup and the dirty-cache
  boot readback test; do not enable no-flush sharing from a cold-pixel check alone.
  Presentation requires verified uncached GPU writes because display does not
  snoop CPU caches. Its dedicated PMM output buffers have WC-only CPU mappings.
  Reject FBC/PSR and unsupported plane layouts; reuse a buffer only after LIVE
  and the frame counter confirm the other buffer latched. Failed flips restore
  firmware scanout or retain the pinned live private buffer for CPU fallback.
- Experimental `GPU_RENDER=1` adds a private logical RCS context and PPGTT
  tables. It enables desktop scaled-client draws only after fence/schedule-out
  and offscreen textured scaling/alpha readback pass. Gen9 HDC noncoherent
  context/operation and CACHE_MODE_1 workarounds precede TLB invalidation in
  every RCS packet. Keep explicit pixel clflush; the no-flush probe is removed.
  RCS recycles its idle context/ring before wrapping. Retain control-line flushing, retirement-
  aware source-map reuse and Gen9/Kaby Lake invalidation sequencing. An RCS
  reset failure requires destination quarantine independently of BCS reset.
  Font/icon rasterization and general alpha-layer integration remain CPU work.
  Gen9 state/context host tests and the mock QEMU scaled-client/failure path
  passed. CPU, page-flip and RCS production kernels build; ISO/USB/recovery
  boot checks pass on VGA. The owner subsequently supplied a physical log passing
  the context fence and textured copy/scaling/premultiplied alpha readbacks,
  followed by a page-flip latch. Scaled desktop correctness/performance still
  need measurements. `GPU_RENDER=1` is opt-in and implies presentation.
- Kernel string helpers are local definitions, not a hosted C runtime.
  Compiler-generated libc calls and the ext4/codec shim need deliberate linkage.
- Interrupt and syscall entry preserve x87/SSE state in aligned stack frames
  before calling C++. Kernel entry uses default floating-point controls; exit
  restores the interrupted controls/registers. Keep this ahead of C handlers:
  optimized UI drawing and the compositor both use XMM registers. AVX is not
  enabled; enabling it requires broader extended-state preservation.
- Do not remove USB protocol fixes as speculative workarounds. Preserve bounded
  reset/probe handling and exact transfer-completion ownership.
- Headers are dependency-tracked; compiler-option changes require `make clean`.
  Close QEMU before updating an image it has open.

## Known limitations

- ELF validation, segment permissions/W^X, user-pointer checks, window-ID
  authorization and malformed-ELF/failure cleanup remain incomplete. Normal
  user-task reap releases loader-owned pages and private page tables. The app API
  is for trusted programs, not a finished security boundary.
- One foreground application per console channel; no process groups, general
  signals, argument/environment passing or dynamic linking.
- No translucent client windows, hardware cursor plane, general modesetting or
  GPU client API. Experimental RCS support requires physical readback validation.
  PNG/font alpha works on the CPU already.
- Stable boot-volume identity, GPT, internal-drive drivers
  and installation remain pending. Permissions, links and UTC file timestamps
  also need defined semantics.
- External FAT32/exFAT file transfers, progress/cancel, timestamps and eject are
  implemented. Runtime timezone/DST configuration is pending. Icon
  themes implement the native subset in [IMAGES](IMAGES.md).
- A failing device can delay sibling HID discovery during synchronous
  enumeration. Surface/ZM3310 wireless stutter is not confirmed resolved.
- Some generated binaries/images are tracked. `.gitignore` does not untrack
  them; review source/docs separately and avoid committing build outputs.
  Keep line-ending normalization separate from functional edits.

## Verification boundary

On 2026-11-19, CPU, `GPU_COMPOSE=1 GPU_PRESENT=0` and `GPU_PRESENT=1`
production kernels built in WSL. The production page-flip image contains no
software test backend and passed ISO, USB and RAM-recovery boot/shell checks.
The GPU host command interpreter passed ASan/UBSan tests. A separate test-only
CPU backend in QEMU verified current rainbow pixels, skipped full CPU generation,
and partial-batch failure with buffer quarantine, source pinning and CPU recovery.
None of these emulator checks execute Intel MMIO or validate physical image
correctness, vblank latching, cache visibility or reset behavior.

The owner's first `GPU_COMPOSE=1` laptop log identifies Intel `0x5916`, a 4 GiB
GGTT and a 256 MiB aperture. Initialization failed before engine startup because
the allocator found no sufficiently large non-present interval. GGTT allocation
now also checks long uniform firmware scratch aliases outside protected display
ranges. The owner subsequently saw `opaque+rainbow`, but reported GPU CPU usage
around 24% mostly covered (CPU mode 8%), 6% hidden, and up to 80% fully exposed.
The updated revision configures/verifies coherent LLC sharing and reuses source
bindings, avoiding bulk pixel-cache flushes and repeated MMIO mappings. It also
keeps composition-mode boot tests offscreen. The owner subsequently confirmed
about 8% CPU mostly covered and 24% fullscreen versus 60% fullscreen on CPU.
The new experimental `GPU_PRESENT=1` stage and fullscreen retained damage path
have host/mock tests. After fixing the SURFLIVE address comparison, the owner
reported about 5% CPU fullscreen and no visible tearing. Warm-restart init
failure is suspected, pending a failed-boot log. They add same-mode
page flipping, not general modesetting or shader rendering. Do not describe the
earlier allocation failure as unsupported hardware,
insufficient VRAM or a GPU hang.

Earlier desktop/compositor behavior and the Gen9 blitter ran on the test laptop.
The owner confirmed that the discovery grace window restored Surface USB-C root
mounting, and that automatic capacity setup and `mv` worked on a physical 4 GB
stick. These observations do not validate every later driver/desktop change.

Recent host/QEMU checks cover ext4 save/recovery/failure paths, capacity setup,
USB protocol helpers, PNG/JPEG resources, lazy icon lifetimes, recovery-memory
reclamation and default-VGA ISO/USB/recovery boot. The combined build still needs
physical input, persistence, power-action and display verification. See
[TESTING](TESTING.md); do not generalize one result to an entire hardware class.

## Maintaining these docs

Update feature guides when behavior changes, this snapshot when architecture or
constraints change, and the pending-work list when work completes. Investigation
chronology belongs in version-control history. Preserve unresolved limitations
and test evidence instead of appending superseded status entries.
