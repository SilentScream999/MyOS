# Verification guide

Updated 2026-11-17. Run commands from the repository root in WSL/Linux.
Use current build artifacts, clang sanitizers, QEMU, OVMF and e2fsprogs as needed.
Build/setup instructions are in [README](../README.md).

## Image Viewer and retained Start icons

```sh
make test-image-viewer test-explorer test-user-memory test-user-entry
python3 tests/run_image_tests.py
make iso PERSIST_DISK=.storage-test/viewer-seed.img ROOT_MIB=64
python3 tests/qemu_image_viewer_test.py
```

For interrupt/syscall register preservation, also run:

```sh
python3 tests/qemu_simd_test.py
```

The native probe keeps distinct patterns in XMM0-15, nondefault MXCSR and live
x87 values across blocking syscalls and timer-driven preemption. It reproduced
failures on both paths before entry stubs saved/restored the extended state.
`test-user-entry` checks the loader's initial stack pointer and jumps onto a
test stack to execute the aligned SIMD store that faulted during Image Viewer
startup when the function-entry return-address slot was missing.
The viewer guest suite also checks solid bands in its plus button and the
shared file-picker text field after repeated repaints, avoiding text/corner
pixels that legitimately differ from the background.
It checks visible Up/Refresh icons in both Explorer and the native picker,
then clicks Refresh and Up to confirm they keep/reload the listing and navigate
to its parent. The host picker test starts from zeroed instance storage to
reproduce native startup without global C++ constructors.

Host sanitizer tests cover PNG/JPEG loads, failed-load preservation, zoom/pan,
partial reads, arena reuse, Explorer image associations and reference-counted
menu icon lifetimes without reads or pixel copies on visibility changes.
The QEMU test boots a disposable copy of the seed and exercises Explorer image
launch, PNG/JPEG display, actual size, invalid-image handling, file dialogs,
maximize/resize, rapid Start reopening and three repeated open/close cycles
that must return physical RAM to within 256 KiB of the warmed baseline. The
ownership-walk sanitizer test checks that borrowed window pages and shared
kernel mappings survive while private tables and loader-owned pages are freed
exactly once. Captures stay in `.storage-test/`.

Run QEMU suites sequentially: the shared harness uses fixed QMP/debug ports.
Keep physical-device tests separate from emulator results. Fixture scripts can
create large private images under `.storage-test`; retain host staging space and
do not use a live/personal system image as a disposable fixture.

## Boot, images and recovery lifetimes

```sh
make iso
python3 tests/run_image_tests.py
python3 tests/run_wiz_tests.py        # or: make test-wiz (.wiz format, desktop, shell, app library)
python3 tests/qemu_boot_framebuffer_test.py
python3 tools/make_usb_image.py --output .storage-test/image-runtime.img --debug
python3 tests/qemu_image_test.py .storage-test/image-runtime.img
```

For an existing `image-runtime.img`, use `--update-boot --debug` rather than
creating it again. The host suite covers PNG variants/Adam7/JPEG, alpha/resize/
clipping, malformed input, allocation failures, manifests/themes, lazy resource
I/O, cache replacement/eviction and safe recovery-tree/module-page reclamation.
QEMU covers default-VGA ISO boot, USB boot, RAM recovery, live icon/manifest
replacement, menu/window resource lifetimes and failed-disk-shell fallback.

## Native File Explorer

External filesystem regression tests include FAT32 and exFAT copies, a sparse
exFAT fixture larger than 4 GiB, RTC timestamp packing, abandoned-stage cleanup,
MBR/unpartitioned discovery, busy-drive checks and failed/successful eject.
Replacement tests inject failures at each sector-write boundary and verify a
complete old or complete new document after remount. Explorer tests cover drive
arrival/removal, preserving selection, editing deferral and icon hover feedback.
Formatting is enabled only in the host fixture library.

```sh
make test-fat32 test-exfat test-explorer
python3 tests/run_usb_hub_tests.py
```

```sh
make test-explorer
make test-notepad
make test-console
make iso PERSIST_DISK=.storage-test/explorer-seed.img ROOT_MIB=64
python3 tests/qemu_explorer_test.py
```

Host tests cover the actual userspace file operations and input handling with
ASan/UBSan. The QEMU test uses real pointer/keyboard events, validates `TRYWAIT`,
Start launch, browsing, package launching, file dialogs and persistence, then
checks the resulting ext4 filesystem. See [Explorer](EXPLORER.md) for limits.
The Notepad host suite also covers menu input, browser navigation and filtering,
new-name saves, cancellation, overwrite prompts and failed reads/saves. Its
captures include the File menu and separate Open/Save As browser windows. The
Explorer QEMU suite opens these menus/browser windows and closes them without
changing the fixture document.

## CPU usage and terminal wakeups

```sh
make test-task-manager
make iso PERSIST_DISK=.storage-test/root.img ROOT_MIB=64
python3 tests/qemu_task_manager_test.py
python3 tests/qemu_task_manager_test.py --ps2
clang++ -std=c++20 -O1 -g -no-pie -fsanitize=address,undefined tests/tty_idle_test.cpp -o .storage-test/tty-idle-test
.storage-test/tty-idle-test
python3 tests/qemu_cpu_idle_test.py
python3 tests/qemu_kernel_cpu_test.py
make test-resize test-text-input
python3 tests/qemu_pointer_perf_test.py
python3 tests/qemu_caret_pointer_test.py
```

Task Manager's host test covers PID matching across reordered snapshots, new
and exited processes, CPU sorting, idle samples, failed reads, truncation and
native rendering, group collapse/scroll behavior, CPU-history timestamps and
ring-buffer wrap. Its PPM preview is `.storage-test/task-manager.ppm`. The real
QEMU syscall probe checks kernel/background/foreground/GUI classification,
optional metadata compatibility, invalid buffers/capacities and timestamps.
The UI test saves `.storage-test/task-manager-programs.png` and exercises the
collapsible section headers. The same test checks Ctrl+Shift+Esc launch, focus,
minimized-window restoration, held-key duplicate suppression, modifier release,
and absence of shortcut input in the shell.
`--ps2` uses the PS/2 keyboard instead of USB HID. The icon is a bundled,
transparent CPU-monitor silhouette matching Explorer and Notepad; regenerate
only its seven sizes with `tools/create_default_icons.py --only task_manager`.
Process categories come from task metadata; the shared shell appears under
Background processes. CPU percentages are for the
single scheduled core, and the graph retains up to 60 seconds of valid samples.

The idle host test covers idle intervals interrupted by scheduled work, per-task
cycle accounting across context switches and halts, sample
initialization and raw/canonical/launch/cancellation wakeups. QEMU verifies that
an untouched prompt blocks, Counter returns to the same sleeping prompt, real
input wakes the shell, Start launches a private console without waking the shared
prompt, and a busy loop reports busy CPU time. The HUD
includes desktop/driver work; an idle prompt does not imply a zero-percent OS.

The kernel profiling test clicks Task Manager's Kernel details button, compares
the exclusive category sum to PID 0's actual CPU-cycle counter, verifies interrupt
and scheduler activity, then returns to Processes and verifies the profiling
lease expires. It saves `.storage-test/kernel-cpu-details.png`. This checks the
measurement path in QEMU; identifying a physical laptop's idle overhead requires
that laptop's breakdown. CPU rates from the emulator are not hardware benchmarks.

The pointer test closes the animated fixtures through real controls, then
measures scene compositions and compositor CPU during idle, pointer movement,
ghost dragging and ghost resizing. Pointer-only updates must use the overlay
path; HUD/caret scene damage remains bounded. It also checks that an inactive
terminal generates no caret transitions and drag/resize geometry commits only
on release. `--baseline` records the same measurements without those assertions.
Overlay host tests compare the optimized renderer to a pixel reference and
check that unchanged overlays produce no writes unless scene damage requires
reapplying them. The CPU kernel remains at its configured optimization level;
only the isolated overlay pixel renderer has its own optimized build rule.

The caret/pointer regression places the shell caret's damage rows over a window
title behind the terminal, moves the pointer horizontally through those rows,
and compares the completed saved scene with the terminal surface across multiple
blinks. This catches background pixels left by an inclusive/exclusive bottom-row
mismatch. The captured result is `.storage-test/caret-pointer-passed.png`.

### CPU and desktop regression map (2026-11-16)

- **Explorer header:** `user/explorer.cpp` gives the Name/Type/Size strip its own
  color and borders so it no longer merges visually into the sidebar.
- **Task Manager:** `scheduler.h` accounts scheduled cycles per task and closes
  halt intervals at interrupt entry. CPU percentages describe Core 0. ABI 10 adds
  optional process categories/timestamps without changing older snapshot layouts.
  Kernel details profiles PID 0 only, adds measurement overhead, and expires two
  seconds after its last read. The laptop's reported idle usage was not explained
  by root-port polling; do not treat emulator percentages as hardware targets.
- **Pointer/ghost performance:** empty scene damage now takes the overlay path.
  `overlay_renderer.cpp` has a local optimized build rule; kernel `OPT=-O0` stays
  unchanged. Cached cursor pixels and whole-pixel thin-span stores reduce work.
  Unchanged overlays can skip writes, but scene blits must force their reapplication
  to avoid missing cursor/outline pixels. Check `qemu_pointer_perf_test.py` and
  `window_overlay_test.cpp` if trails, corners or CPU costs regress.
- **Shell caret and thin-line artifact:** inactive/covered carets stop blinking.
  The line exposing a background window came from dropping the last window-copy
  row of a tiny caret repaint. `wm.h` now converts inclusive damage maxima to
  exclusive copy ends; `framebufferstuff.h` converts terminal bounds in the other
  direction, and aligned presentation includes the final pixel. Preserve these
  conversions together. `qemu_caret_pointer_test.py` reproduced the defect before
  the fix and checks completed scene rows across multiple blinks afterward.
- **USB cadence:** root-port scans were restored to the original 8 ms input loop
  (about 125 Hz); the temporary 100 ms root-port throttle was removed.

For rainbow animation visibility and repaint cost:

```sh
clang++ -std=c++20 -O1 -g -no-pie -fsanitize=address,undefined tests/visibility_test.cpp -o .storage-test/visibility-test
.storage-test/visibility-test
python3 tools/make_usb_image.py --output .storage-test/rainbow-base.img --debug
python3 tests/qemu_rainbow_test.py
```

Refresh an existing fixture with `--update-boot --debug`. Host checks compare
exposed coverage with a pixel mask, including clipping and bounded-region
overflow. QEMU drags actual windows, verifies fully covered animation stops and
resumes on exposure, and records CPU/frame costs and screenshots. Timings depend
on the host; this does not measure physical GPU performance.

## Experimental GPU composition

For the render-engine state/context regression, run `make test-gpu-render`.
The ASan/UBSan test decodes Gen9 texture/alpha fields, checks clipped texture
coordinates and sentinels, validates logical-context register slots, and checks
foreground subtraction for scaled clients. It does not execute GPU shaders.
Build with `make iso GPU_RENDER=1` for the physical offscreen RCS readback gate.
Keep its failed RCS register diagnostics if the hook is rejected.
On the laptop, launch `run /bin/win_test.elf` and resize or maximize its fixed
480x320 canvas to exercise RCS scaling. The owner ran the reset-handshake
baseline for 800 seconds without a freeze; that baseline restricted ordinary
scaled windows to CPU and is preserved at `6baa52b`. The current revision
restores ordinary resized-client RCS at the owner's request. Verify odd sizes,
wide/short and tall/narrow shapes, minimizing/restoring, and sustained fullscreen
use. Place Rainbow and the demo side by side and compare Compositor CPU usage
with the saved baseline. Check the gradient, animated top bar,
click stamps, foreground occlusion and restore. Compare Compositor CPU usage
at identical geometry with `GPU_RENDER=0`. Rainbow uses the existing BCS path.
Gen9 RCS uses explicit pixel flushes and emits the non-coherent HDC and render
cache workarounds before invalidation. Look for `GPU: Gen9 RCS hang workarounds
enabled; explicit pixel handoffs`. The earlier no-flush probe is removed.
Look for `GPU: idle RCS ring recycle completed` after enough submissions.
RCS reset now requires the engine's ready-for-reset handshake before each
domain reset. The host regression exercises repeated prepare/ack/reset/cancel
cycles and refuses domain writes when preparation times out. Neither those
checks nor QEMU establish physical reset or sustained render stability.
QEMU also maximizes the actual demo and checks that its
partial animation uses the retained scaled path before injecting render failure.
It also checks adjacent ordinary RCS/BCS clients use retained damage, their
pixels and CPU resize grips remain correct, and an undamaged sentinel on the
same scanline is preserved. Host coverage checks wallpaper gaps and requires
full CPU repaint for damaged chrome or unsupported foreground content.
The real demo is pointer-resized to 639x477 before maximizing, and the test
requires its fixed-canvas source to reach RCS. An animation crossing the desktop
HUD must preserve HUD pixels, retain client damage below the top band and
present only once. The HUD band uses the complete scene renderer; maximized
applications continue to hide desktop statistics.
The render host test also checks 2,048 fixed-size ring packets across wraps.
The demo publishes immutable snapshots and carries pending damage across a
deferred publish; the optional publish height preserves full updates for old
clients when zero. Physical sustained RCS stability remains a separate check.
Normal builds now probe supported composition/page flips automatically. Host
tests need no GPU flags; use `GPU=0` for an explicit CPU-only build.

```sh
make test-gpu
make clean
make iso GPU_COMPOSE=1 GPU_PRESENT=0 PERSIST_DISK=.storage-test/gpu-hardware.img ROOT_MIB=64
```

The host test interprets the actual BLT command stream against a pixel
reference for stacked/clipped clients, unaligned/scaled CPU fallbacks, rainbow
phase wrap and row replication, command bounds, ring wrap, operation-budget
overflow and GGTT reservations. Firmware-layout regressions include a fully
present shared-scratch table, a unique PTE inside an otherwise uniform run,
invalid/all-ones reads, framebuffer-page exclusions, programmed/live display
bases, high cursor addresses, and rejection of compressed/tiled/rotated or
unknown display formats. Mapping-cache tests check steady-state reuse, address,
size/order changes, palette-only work, retirement-epoch invalidation, and
preservation of all other private-PAT entries when selecting entry zero.
Display tests cover guarded plane selection, complete-frame/overlay pixels,
no writes to the visible frame, no pending/front-buffer reuse, frame-count wrap,
timeout state and retained fullscreen damage with intersecting foreground apps.
ASan/UBSan are enabled.

To test WM integration without Intel hardware, use the explicit test backend:

```sh
make clean
make iso CXX='clang++ -DGPU_COMPOSITOR_TEST -DGPU_PRESENT_TEST -DGPU_RENDER_TEST' PERSIST_DISK=.storage-test/gpu-test.img ROOT_MIB=64
python3 tests/qemu_gpu_compositor_test.py
```

That backend lives in `tests/` and is absent from normal kernels. The test reads
the master buffer at completed frame boundaries, verifies current rainbow
pixels and skipped full CPU surface generation, then injects a partially
executed batch failure that switches to an empty replacement master buffer.
It also launches/closes a native window and checks that retirement invalidates
cached bindings, then checks hook disablement, resumed CPU animation and
continued pointer input after failure. Actual Rainbow maximize exercises the
retained fullscreen path. The optional software presenter checks one complete
presentation per frame and display failure while preserving GPU composition.
These are software integration results, not GPU performance or reset tests.
Rebuild without either test macro before physical testing.

For experimental page flips, build `make clean && make iso GPU_PRESENT=1`.
Look for HUD `GPU:opaque+rainbow flip UC` and Boot Log
`GPU: first vblank page flip latched`. Unsupported plane/power/cache layouts
retain CPU presentation; preserve the `GPU display` diagnostics in a fresh log.
Check fullscreen Rainbow's moving tear, cursor/drag outlines, menus, native
windows, smoothness/frame rate and CPU usage. QEMU's mocked presenter cannot
validate Intel vblank, cache visibility or actual scanout rollback.

For physical LLC/CPU-presentation testing, use `GPU_COMPOSE=1 GPU_PRESENT=0` and look for
`GPU: offscreen composition readback PASSED` followed by
`GPU: dirty CPU/GPU cache coherence PASSED - bulk clflush disabled` and
`opaque GPU composition enabled; CPU presentation` in Boot Log. The HUD shows
`GPU:opaque+rainbow LLC`. A failed cache-policy/coherence test retains CPU
composition; the earlier flush-every-frame path is no longer enabled at runtime.
Record device
ID, resolution/pitch, CPU usage and frame times with Rainbow exposed, partially
covered and fully hidden, plus dragging/resizing, cursor/ghost outlines, menus,
native apps and repeated window close/reopen. Compare with the same build and
workload using `GPU=0 GPU_COMPOSE=0`. The owner's previous revision regressed
from roughly 8% CPU in CPU mode to 24% in GPU mode with Rainbow mostly covered,
about 6% hidden and up to 80% exposed. Those observations do not validate the
new page-flip stage. The owner subsequently reported about 8% mostly covered
and 24% fullscreen versus 60% fullscreen on CPU after the coherence changes.

If initialization reports `no unused GGTT interval for owned buffers`, retain
the new `GPU GGTT` and `GPU firmware display` lines: these report required space,
largest non-present/alias runs, protected ranges and representative PTEs.
The previous non-present-only allocator hit this condition on the `0x5916`
laptop. The owner subsequently reported the composition-enabled HUD, but the
new page-flip stage still requires a physical retest.

The older `qemu_rainbow_test.py` still assumes a legacy terminal pointer and its
dragging fixture needs updating for native console hosts. Do not count it as a
passed GPU regression. The new host tests cover visibility and the integration
test covers GPU-hook animation and failure recovery.

## Storage and capacity

Prepare the dedicated ext4 fixture before its dependent tests:

```sh
python3 tools/make_usb_image.py --output .storage-test/ext4-usb.img --fresh
python3 tests/run_ext4_tests.py
python3 tests/qemu_ext4_test.py --hub-depth 2
```

The ext4 runner exercises the real adapter with ASan/UBSan, failure containment,
cold journal recovery, atomic package exchanges at 24 injected write boundaries,
and Linux filesystem checking. The QEMU suite writes its
fixture across repeated boots, including unclean poweroff and normal shutdown.

For package-specific checks, use a freshly built fixture containing the sample
packages (the QEMU test boots its own disposable copy):

```sh
make all
make test-wiz
python3 tools/make_usb_image.py --output .storage-test/ext4-usb.img --update-boot
python3 tests/qemu_wiz_packages_test.py
```

The host suite covers checksum detection throughout the executable and payload,
legacy v1 reading, first-entry duplicate semantics, failed upgrade publication,
57/64-character IDs, desktop discovery/icons and the bundle library. QEMU checks
portable execution, install/reinstall through the actual exchange syscall,
inspection, pinning, removal, purge and preservation of saved app data.

| Command | Coverage |
|---|---|
| `python3 tests/ext4_image_test.py` | Creation, update preservation, invalid roots/features, atomic publication |
| `python3 tests/atomic_save_test.py` | Injected save failures and complete old/new destinations |
| `python3 tests/capacity_image_test.py` | Offline image expansion, preservation and bounded host memory |
| `python3 tests/run_autogrow_tests.py` | First-boot format/copy/publication failure and retry, policy/UUID checks |
| `python3 tests/qemu_autogrow_test.py` | Automatic larger root and fixed DEBUG policy across boots |
| `python3 tests/qemu_storage_api_test.py` | Actual save/file syscalls and cleanup |
| `python3 tests/qemu_storage_async_test.py` | Input/timer/desktop progress during a missing disk completion |
| `python3 tests/qemu_power_action_test.py` | Shutdown/restart with connected and unplugged boot storage |
| `python3 tests/qemu_power_action_test.py --menu` | Start menu power buttons, screenshots, clean filesystem shutdown and saved-data checks with connected/unplugged storage |

## USB and fonts

| Command | Coverage |
|---|---|
| `python3 tests/run_usb_hub_tests.py` | Protocol helpers, scripted actual enumeration/reset/wait/write paths, completion ownership |
| `python3 tests/qemu_late_storage_test.py` | Discovery grace and fixed root after recovery starts |
| `python3 tests/qemu_usb_recovery_test.py` | Root-failure report and topology diagnostics |
| `python3 tests/qemu_font_selection_test.py wyzar-usb.img assets/fonts/selawik/selawk.ttf` | Custom/missing/corrupt TTF selection and recovery font |

Other pointer/drag/RTC fixtures may exist locally but are not all tracked. Do not
make them prerequisites for a fresh checkout. See [USB_INPUT](USB_INPUT.md) and
[FONTS](FONTS.md) for the corresponding implementation boundaries.

## Physical evidence

The owner has reported earlier desktop/Gen9 blitter operation, restored Surface
USB-C root mounting, and automatic capacity setup/`mv` on a 4 GB stick. Current
full-build input, storage latency, wireless stutter, power actions and display
performance still need targeted hardware checks. QEMU does not test Intel GPU
registers or physical USB 3 links. Kernel-wide `OPT=-O2` runtime verification is
separate from the optimized image codec.

For failures, retain `/bootlog.txt`, recovery `/run/storage.txt` and relevant
QEMU stop/register state or slow-motion display footage. A GUI Paused state can
mean a guest triple fault/shutdown; do not assume it is an ordinary OS wait.
Tests demonstrate the listed cases, not every power-cut point or hardware model.
