# Verification guide

Updated 2026-11-17. Run commands from the repository root in WSL/Linux.
Use current build artifacts, clang sanitizers, QEMU, OVMF and e2fsprogs as needed.
Build/setup instructions are in [README](../README.md).

For journal initialization on reused media, run
`python3 tests/journal_reformat_test.py`. It creates an older journaled
filesystem, reformats the same blocks, provisions new files without journaling
as automatic expansion does, and repeatedly interrupts otherwise idle mounts.
Fresh processes must recover the new marker and document unchanged. Independent
`e2fsck -fn` checks validate the final metadata. Fixtures are disposable files.

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

To verify the explicit CPU-only configuration with a disposable disk:

```sh
make iso GPU=0 PERSIST_DISK=.storage-test/cpu-only.img ROOT_MIB=64
python3 tests/qemu_boot_framebuffer_test.py --image .storage-test/cpu-only.img
python3 tests/qemu_gpu_window_test.py
make iso
```

The boot suite checks that GPU hooks remain absent in ISO, USB and RAM recovery
boots. The window suite exercises animation, alpha, resizing and Start close.
The final build refreshes the standard image while preserving its saved files.

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

Rainbow batching host coverage checks exposed-region bounds and preservation
of opaque GPU occlusion. The QEMU suite creates a smaller opaque window inside
Rainbow and requires a shared blit scene across exposed strips. It also places
alpha over Rainbow across the HUD and requires one RCS scene per active frame
(allowing an independent periodic HUD refresh), unchanged HUD pixels and one
presentation. The general damage-list tests still preserve L-shaped regions;
only GPU Rainbow's scene request is coalesced. CPU mode is unchanged.

`make test-scheduler-deadline` checks prompt compositor timed wakes, untouched
manual/event waits and ordinary task deadlines, dead tasks, and preservation
of ten-ms quanta under serial one-ms compositor sleeps. GPU integration and
timeout recovery must still pass. After a production build, run
`python3 tests/qemu_cpu_idle_test.py` to check blocked input, foreground busy
work, cancellation, CPU accounting and private desktop launch after the timing
change. On hardware compare the new GPU perf loop/submission timings with the
previous ~30 loops/s opaque and slowest ~15 loops/s alpha observations.
The idle suite derives the current private-console foreground pointer as well
as Task layout. `python3 tests/qemu_simd_test.py` checks XMM0-15, MXCSR and x87
across syscalls/timer preemption; its probe reports the computed result in a
temporary window, independent of the terminal frontend's private text buffer.

The render host suite checks immutable staging reuse, publication-slot address
reuse with a new revision, epoch invalidation, and fresh uploads for mutable
buffers. The QEMU RCS backend samples staged bytes and checks that repeated
foreground repaint reuses one upload, then reads fresh pixels when the same
slot publishes a new revision. These tests do not measure Intel performance.
For the reported overlap regression, compare manually fullsized Rainbow alone,
under the small opaque GPU test, under the enlarged opaque test, and with A.
Hold each for at least five seconds and save Boot Log. `GPU perf:` records
separate uploaded KiB/reuse and elapsed scene/present/BCS/RCS/source/handoff
phases. Phase times include sleeping/preemption and are not CPU percentages.
Each sample now has three adjacent records with the same `window=...ms`:

- `GPU perf:` retains the scene/present/BCS/RCS/source/handoff totals and adds
  `cpuAlpha` (CPU alpha draws), `alphaKiPx` (destination pixels divided by 1024)
  and `alphaMs` (elapsed CPU blend time). Production alpha remains on SIMD
  at all supported sizes; opaque scaling may still submit RCS work.
- `GPU present:` counts presentation attempts and separates elapsed `copy`
  (full-frame master-to-hidden-scanout BCS copy including its fence), `overlay`
  (cursor/drag-outline CPU drawing plus its store fence), and `flip` (arming
  the surface through observing its LIVE/frame-counter latch).
- `GPU sleep:` reports `bcs=N/ms/slow2/slow10`, likewise `rcs` and `flip`.
  `N` is the number of existing one-ms sleep calls, `ms` is their combined
  elapsed return time, and `slow2`/`slow10` count returns taking at least 2/10 ms.
  Divide `ms` by `N` for average sleep return time. Zero calls means that path
  did not sleep. These exclude the compositor's normal frame-cadence sleep.

All durations are totals for that sample, include preemption, and are not CPU
utilization. Copy time includes BCS wait/sleep; flip time includes flip sleep.
Those nested totals must not be added together. No MMIO reads, GPU commands,
cache handoffs, timeouts or sleep policy were added or removed for measurement.
The limit is 32 samples per boot.

`make test-gpu-perf` checks sleep threshold boundaries and sample subtraction.
Periodic GPU summaries and launch/menu reports require `GPU_WORKLOAD=1` or
`GPU_FRAME_DIAGNOSTICS=1`. Normal builds still report GPU startup checks and errors.
Use the measurement flags in [BUILD_CONFIGS](../BUILD_CONFIGS.txt) to
reproduce the Kaby Lake measurements on the standard `wyzar-usb.img`. The
default rendering path checks multi-band alpha readback before enabling desktop
RCS. Use `GPU_RCS_ALPHA=0` for a SIMD translucency comparison.
Leave Rainbow fullscreen for five seconds, then
keep a medium transparent window stationary above it for at least six seconds.
Each stable layout gets two consecutive two-second samples.
Match the layout IDs in the two `GPU Rainbow:` lines with the preceding
presentation samples. Each trace follows the frame with the longest flip
interval. Stage timestamps are microseconds since Rainbow updated. The RCS
span runs from the first alpha submit to the last observed retirement and
can include other work between draws. A dash indicates an unobserved stage.
`GPU window:` lines describe the same slowest frame as the preceding
`GPU Rainbow:` line. Window IDs and titles identify stack-order visits.
`startUs` and `endUs` are offsets from Rainbow's update. Shadow, chrome,
GPU planning/submission, and remaining client drawing have exclusive elapsed
times. `bcsUs` and `rcsUs` are nested driver times, including sleeps.
Repeated visits to a window are accumulated in `passes`. Gaps between window
visits and the final arm timestamp belong to other compositor work.
Shadow drawing subtracts higher opaque windows and opaque title bars before
blending exposed strips with SSE2. Compare `shadowUs` with identical window
sizes and positions, including shadows visible through translucent clients.
Experimental builds emit up to 32 `GPU launch:` records. Times start when
`launch_program` begins classification/loading, excluding launcher queue delay.
The records show task readiness, first owned window creation, first surface
damage/publication, and the first confirmed GPU flip following a compositor
snapshot of that painted window. Occlusion may hide the window on that flip.
The first startup second records direct/copy counts and the longest flip gap
independently of layout sampling resets. Slow startups log after their first
flip or after five seconds, with missing milestones shown as dashes.
The default GPU build enables direct scanout after
`GPU: direct scanout BCS/RCS readback PASSED`. Every window and desktop layer
uses the same presentation path, including Start, clipboard, and taskbar popups.
Each scanout accumulates scene damage until its next use. Only the hidden
buffer's pending regions and previous cursor/outline pixels are recomposed.
The pending list clears after a confirmed flip. Both buffers are seeded once
from the retained scene. Returning from direct rendering to CPU or copy
presentation requires a complete retained repaint.
`GPU frame:` records direct flips, repair rectangles/pixels, and seed-copy
bytes independently of Rainbow. Menu, clipboard, taskbar-popup and focus
changes restart bounded sampling. Test app animation with Rainbow closed,
Task Manager updates, Explorer hover, menus, resizing, moving, minimizing,
restoring and closing windows, and cursor movement over previously damaged
regions. Check for stale pixels after alternating buffers.
Explorer publishes immutable completed frames for full repaints, hover
updates, text fields and caret changes. Failed publications retry through
the normal repaint loop.
Start retains its rendered pixels in a reusable WB scratch surface. Hover,
geometry, font scale and resource-epoch changes rebuild the cached image.
Its opaque panel also occludes wallpaper, window shadows and ordered GPU
client draws beneath it.
`GPU frame:` includes elapsed `menuMs`, `menuRenders` and `menuReuses`.
Compare stable menu-open samples with hover samples and check icon/text
updates after a resource refresh. The scratch allocation is capped at 64 MiB
and falls back to direct menu rendering if unavailable.
The experimental build traces the first eight Start openings independently
of the two-second samples. After one second, `GPU menu-open:` reports resource
request/build/commit, cache construction and first presentation offsets from
menu activation. Negative offsets mean a resource build began before opening.
Missing milestones are dashes.
The cache timestamps span the first and last observed builds, while
`buildUs` accumulates their durations. A BCS wait that began before opening
can have a negative `maxBcsStartUs`.
`GPU menu-cost:` reports allocation/build time, resource-worker scheduled CPU
time, the longest BCS wait's wall/CPU/sleep times,
and the longest presentation gap. Sleep time includes wakeup scheduling delay.
These counters measure observed completion and scheduled time, not hardware
execution duration. Test first open, quick reopen, then reopen after six seconds.
The menu scratch buffer and visible-row menu icons are prepared during boot.
Background resource refresh offers scheduling checkpoints at file/directory
operations and icon decode/resize boundaries. After roughly 0.5 ms of elapsed
work it sleeps for one tick, letting the compositor run. An individual I/O or
decode operation can exceed that interval. The current immutable catalog stays
available until the refreshed catalog can be published between readers.
Eligibility failures appear as `GPU direct:` lines.
Retained scenes can refresh exposed Rainbow pixels on the retained hidden
scanout with one BCS plan. Foreground clients and chrome stay in that scanout.
The refreshed rectangles receive their overlapping shadows again after the
GPU completes, avoiding accumulation while leaving static window pixels alone.
Translucent layers above those rectangles reblend through RCS in stack order,
with the existing CPU resize-grip strip. Alpha over static foreground pixels
stays cached. Input/app damage is repaired only outside the refreshed animation
rectangles, so a translucent update is not drawn twice over the same pixels.
Source snapshots are refreshed after repairs. A changed layout uses a complete
ordinary scene before presenting. Uncached Start panels, clipboard popups, unsupported scaled animation surfaces,
unavailable GPU alpha and visibility-list overflow use ordinary composition.
Old scanout cursor/outline damage is still repaired before presentation.
`GPU frame:` includes `animationFrames`, `animationKiPx` and `alphaFrames` to
confirm this path actually ran. Expect nonzero counts with Explorer or GPU
translucency over Rainbow, fewer foreground copies and improved flip cadence. Moving,
resizing, typing and opening/closing windows must keep their pixels correct.
The sanitizer command-stream test compares 90 alternating-buffer updates
against complete scene redraws, including three overlapping opaque windows
and shadows crossing static foreground pixels. It also rejects fragmented
regions when their visibility storage overflows. Additional 90-frame tests cover
two scaled translucent layers, overlapping opaque windows, CPU grip strips,
foreground-content changes and exact repair cutouts. Intel timing needs hardware.
Start uses its ordinary cached-menu composition path again. Retaining it as
another animation occluder fragmented repairs and added BCS submissions on
hardware. Default menu-open samples therefore have `animationFrames=0`;
the `GPU_RETAINED_MENU` path described below uses cached replay.
The experimental RCS build can cache read-only texture sampling through a
separate MOCS entry. Render destinations keep their existing uncached policy,
and upload flushes, texture invalidations, completion and retirement checks
remain intact. Entry programming is read back, then a boot probe replaces the
same source address eight times with transparent/half-alpha/opaque pixels and
checks 1x/2x sampling and source-over against exact CPU results. A pixel mismatch
selects UC sampling before the normal RCS readback tests. A GPU timeout follows
the existing GPU failure/fallback path. Look for `RCS cached texture source
readback PASSED (8 updates)` and `texture sampling selected L3+LLC WB;
destination UC`, or the UC-selection message, before comparing `rcsUs` and
flip cadence under transparency. This optimization shortens sampling work;
it does not remove the ordering required between BCS and RCS.
The later cached-render-target boot experiment was removed after a hardware
timeout at `RTCLEAN` (sequence 0x2D) disabled GPU rendering before the desktop.
That checkpoint confirms RT cleanup completed, but the subsequent drain and
completion sequence did not. No target-cache benchmark completed. Keep targets
UC and preserve the previous GPU path; do not suppress timeout/retirement checks
or treat the resulting CPU-only session as a GPU performance measurement.
`GPU_WORKLOAD=1 GPU_RCS_ALPHA=1` adds CPU-side accounting to the existing
execution-timing build. It does not add GPU commands, MMIO operations, cache
policies, fences, probes or ownership transitions. The default is off.
`GPU workload:` accompanies the `GPU execution:` lines for the same sampled
frame, selected by the largest flip interval within the layout sample.
`other`, `rainbow`, `client`, `scanout` and `fill` are `operations/bytes`.
Copy bytes are logical BGRA payload read AND written: a 1 MiB copy means
1 MiB of source payload and 1 MiB of destination payload, not 1 MiB of measured
bus traffic. Repeated/self copies count each time; pitch padding is excluded.
Rainbow counts the BLTs emitted by its tiled/row replication algorithm rather
than assuming a single high-level rectangle is a single BLT. Other includes
copies outside ordinary scene clients and scanout, such as staged RCS copies.
`fences` counts encoded BCS fences, including existing diagnostic fences.
BCS accounting records accepted commands; RCS counts successful submissions.
`blendPx`/`opaquePx` count actual RCS rectangle areas after gap clipping;
alpha-zero texels still inside those rectangles count as rasterized pixels.
These fields are not unique pixels, GPU utilization, or measured DRAM traffic.
Counters attach only during compositor frames; boot probes are excluded.
`bcsSamples` versus `bcsBatches` and `rcsSamples` versus `rcsDraws` indicate
timestamp coverage: do not divide all-frame payload by partial timing samples
to claim a bandwidth figure. CPU presentation or no timestamped flip can
produce zero sampled workload; it does not establish zero desktop work.
For hardware comparison keep Rainbow geometry fixed and hold each state for
at least ten seconds: alone, covered by an opaque GPU Test, then the same GPU
Test geometry with alpha enabled. Save `/bootlog.txt` after all three states.
Use workload and existing GPU times to identify repeated copying/blending
before changing the rendering path. `make test-gpu-perf` checks instrumentation
against decoded BLTs and verifies an identical ring with accounting detached,
plus alpha rectangle totals and retention with the matching timing snapshot.
`GPU_OPAQUE_RETAIN=1` enables conservative opaque cutouts on the retained
animation path and follows `GPU_RCS_ALPHA` by default. Published immutable premultiplied
surfaces are summarized into at most eight source rectangles. Every source
texel inside a rectangle must have alpha 255. Only the longest fully opaque
horizontal run per band is kept; adjacent identical runs merge. Mapping shrinks
each edge by a source texel and rounds inward, and excludes CPU resize-grip rows.
Source address, revision, dimensions and pitch key the cached proof. Mutable or
unpublished buffers prove nothing. Scans run outside the snapshot IRQ guard in
an optimized helper, with a four-million-source-pixel cap.
These cutouts remove both background animation and subsequent reblending in
those pixels. Existing source damage repairs their retained final pixels.
Mapped coverage contributes to the retained-layout signature: a change during
blocking repairs triggers ordinary scene recomposition. Fragmentation overflow
also uses ordinary composition, preserving GPU readiness. GPU command encoders,
cache policies, ownership barriers and boot probes remain unchanged; only the
submitted geometry and retained pixels differ.
`GPU frame:` adds `opaqueRetainedKiPx` (actual background area removed),
`opaqueScans` and total `opaqueScanUs` per observation window. Compare the
existing `GPU workload:` Rainbow bytes and `blendPx` with the preceding build,
as well as BCS/RCS times and presentation cadence. Hardware gains are unverified
until the owner repeats the same opaque-to-alpha GPU Test overlap.
The sanitizer compositor suite compares both original and optimized plans
against complete redraws over 90 alternating-buffer frames, including stacked
translucency, shadows, source opacity changes, moves, resizing and stack changes.
Coverage tests check integer and floating-point nearest sampling across scales,
negative origins, pitch padding, grip exclusion, revision invalidation, mutable
source rejection and unchanged-plan fallback on fragmentation overflow.
`GPU_ALPHA_MERGE=1` coalesces alpha clip rectangles before desktop submission.
It follows `GPU_RENDER` by default. Boot readback probes retain their original
geometry. Rectangles merge only when their union is exactly a rectangle with
a complete shared edge, for the same staged source and client sampling transform.
No gap is filled and no extra pixel is rasterized. Repeated edge joins handle
bridges between earlier fragments. Capacity is checked after merging the
combined existing/new group; failure leaves the existing group untouched.
The existing 16-rectangle/4 KiB batch and all cache/fence/retirement rules remain.
`test-gpu-render` compares exact per-pixel coverage, holes, shared edges,
sampling coordinates and transactional overflow. A retained-alpha fixture
reduces 18 rectangles in two submissions to four in one, with identical coverage.
Compare hardware `rcsDraws`, `rcsRects`, RCS execution `samples`/`tailUs`, flip
arming time and skipped refreshes in the same translucent overlap. `blendPx`
and Rainbow payload should stay unchanged for identical geometry; reduced
submissions are a host-verified opportunity, not proof of 60 FPS on Intel.
With `GPU_WORKLOAD=1`, `GPU geometry:` records visible windows from Rainbow
upwards in bottom-to-top order, once per stable-layout observation. The snapshot
must match the observation's layout signature; it is copied under the IRQ guard
and printed after releasing it. Coordinates are exclusive `x1,y1,x2,y2` for
outer/client bounds, with source dimensions, alpha policy and maximize state.
`coverPx` is the ordinary opaque window/title-bar intersection with the visible
Rainbow client. `newlyHiddenPx` removes double counting against earlier covers.
`lowerAlphaClientPx` sums geometric intersections with lower translucent client
bounds. It is not a count of alpha texels/blended pixels and does not include
translucent clients above that window. These metrics precede opaque-source
cutouts; use `opaqueRetainedKiPx` and actual workload to account for those.
Explorer can hide a large Rainbow area outside its overlap with GPU Test, so
small apparent overlap with GPU Test alone cannot establish a culling bug.
The next log should compare identical Rainbow/GPU Test geometry before and
after opening or moving Explorer. Capture both layouts for ten seconds each.
The geometry logger changes no rendering plans, GPU commands or cache policies.
`test-gpu-perf` validates area/union accounting against a painted visibility map,
including slight alpha-client overlap with large background coverage, overlapping
opaque windows and clipping at screen edges. Intel occlusion still needs the
owner's window coordinates and hardware workload log.
`GPU_ALPHA_REPAIR_BATCH=1` enables retained repair batching and follows
`GPU_RCS_ALPHA` by default.
Only the topmost visible immutable premultiplied client may defer its final RCS
repair draw. Ordinary composition still repairs its background immediately.
Deferred rectangles must lie inside the GPU client body, exclude resize-grip
rows, and be disjoint from the animation update and every queued repair.
Shell/popup/HUD repairs and layers with visible windows above them render in
their original order. Collection stops before retained-animation preparation.
Before GPU animation submission, revalidate the window id, source address and
revision, dimensions, pitch, client/outer geometry, resize policy and layout.
Overlapping repairs, overflow or changed snapshots cancel the pending work and
recompose the complete scene normally before presentation, preserving GPU
readiness. No incomplete deferred frame may be presented. Successful repairs
join the top client's retained alpha plan, using the existing exact rectangle
merger and unchanged source/cache/fence/context-retirement rules. All other
layers render in their original order. Boot probes and batch capacity are unchanged.
`GPU alpha repair:` reports successful `frames`, queued `rects` and `cancels`
per layout observation window. Compare main-case RCS submissions/tails and flip
arming time against the preceding build. A nonzero counter confirms the path
actually ran; fewer submissions and 60 FPS still require hardware evidence.
Host tests verify repair plus animation fits an existing batch, guard failures
for mutable/non-topmost clients, changed revisions/geometry, grip/animation
overlap, repeated repairs and overflow. The complete-redraw comparison adds a
90-frame alternating-buffer run with changing static background beneath stacked
alpha, delayed final alpha repairs, shadows and layout/source changes.
`GPU_EXACT_REPAIRS=1` retains up to 64 disjoint pending rectangles independently
for each scanout, instead of repeatedly compacting that history into eight
bounding boxes. It subtracts existing coverage before appending and joins only
exact full-edge unions. HUD/body/taskbar intersections are split so full-width
shell writes do not widen the entire height of a crossing client repair.
The retained repair planner consumes that region directly. A conservative
eight-rectangle list remains available for ordinary composition fallback.
Overflow promotes the pending region to complete-screen repair; it never drops
damage or disables the GPU. Successful flips clear only their own pending region;
ordinary master-copy presentation invalidates/clears both. Cursor/outline
restoration and all actual scene damage are still included. Upstream damage
already compacted before scanout tracking remains conservative.
`GPU_RETAINED_MENU=1` permits retained animation with an already valid Start
pixel cache. It does not add the menu as a background occluder. Normal pending
repairs paint the menu first where needed; after GPU layers retire, cached
opaque menu pixels replay only where the animation overwrote them. This avoids
the earlier menu-cutout fragmentation while preserving existing ordering,
opaque-source retention, alpha batching and cursor presentation.
Opening or changed hover/resource/geometry cache keys use ordinary composition
until the cache is valid. The retained layout includes the menu key. A changed
menu/cache during GPU work triggers complete recomposition before presentation,
without treating the input change as a GPU fault. No GPU ownership/cache/fence
rules, encoders or boot probes change. Both flags follow `GPU_RCS_ALPHA`.
`GPU repair plan:` reports `exactFrames` and `exactKiPx` for planned retained
repairs, plus successful cached `menuFrames` and copied `menuKiPx`. The older
`repairKiPx` counter describes the conservative fallback damage list; actual
BLT/raster work is still measured by `GPU workload:` and GPU execution timing.
Host tests check exact pending history and old-overlay restoration over 80
alternating-buffer frames, shell-crossing bounds, disjoint unions and lossless
overflow. Complete-redraw tests add 90 menu-replay frames with changing static
background beneath stacked alpha, shadows, menu open/close/move and hover-color
cache changes. QEMU window checks also exercise real Start opening, changed
cached hover pixels and Escape closure through CPU fallback. Intel cadence,
transition stutters and CPU cost still require the owner's hardware log.
The opaque taskbar app picker uses a cutout shared by Rainbow and its derived
alpha plans. It is replayed in full after the retained window layers with its
own saved/restored clip; ordinary opaque-alpha repairs cannot replace it.
Popup geometry changes during GPU work retain complete-scene recovery.
The mock compositor test corrupts every popup pixel, leaves a partial repair
clip, and compares the final full panel, including hover, text and icon edges.
Its destination-index checks use mock memory, not physical Intel scanouts.
`GPU_FRAME_DIAGNOSTICS=1` adds observation only, default off. `GPU planning:`
reports microsecond `wall/cpu` pairs from TSC and the compositor task's existing
scheduled-CPU accounting: initial preparation, direct-scanout setup, exact
pending union, conservative fallback packing, repair-region subtraction,
ordinary repair drawing, source/layout revalidation, cache construction,
cached-menu replay and full-scene fallback. `direct` includes `merge`/`pack`;
`repairs` or `fallback` can include `menuBuild`. These are nested, not additive.
`invalid` rejects backwards clocks/counters rather than reporting wrapped costs.
`sample=worst` matches the existing worst CPU timeline; `sample=gpu` matches
the existing sampled GPU execution/workload frame. Compare like samples.
`GPU phase:` records `valid/frame/rawScanline/liveSurfaceAddress` at the previous
observed latch, frame start, before and after the existing SURF write, and
observed successful latch. The before/after reads use the existing arm IRQ guard.
Each snapshot brackets four read-only MMIO accesses with TSC and reads the frame
counter twice; a frame transition or all-ones read marks it invalid. No retries,
new waits, rendering commands, mode changes or new readiness checks are added.
`armWindowUs` brackets the original SURF store and posting read; `maxReadUs`
reports the longest snapshot duration so observer overhead stays visible.
The raw scanline mask/registers follow Linux i915
[display registers](https://codebrowser.dev/linux/linux/drivers/gpu/drm/i915/display/intel_display_regs.h.html).
Boot prints pipe and eDP transcoder configuration/HTOTAL/VTOTAL/VBLANK raw
values because the active pipe may feed the eDP transcoder. Do not assume
either timing set is active or derive a precise deadline from a scanline alone:
[i915 scanout timing](https://codebrowser.dev/linux/linux/drivers/gpu/drm/i915/display/intel_vblank.c.html)
applies platform/output-specific offsets. Diagnostics never gate GPU success.
This flag also enlarges the boot log from 64 KiB to 256 KiB; extra-size and
normal-size live-log exports are checked by `make test-gpu-perf`, along with
CPU-vs-wall accounting, disabled timers, frame-crossing snapshots, read counts,
invalid MMIO data and matched-frame retention. Standard builds retain 64 KiB.
For the next hardware comparison resize once, then keep Rainbow and GPU Test
fixed through transparency alone, Start open and Start closed, holding each
state for at least ten seconds. This is an instrumented run, not a speedup claim.
The experimental build also profiles pixel-shader dispatch limits 16, 32 and
63 on dedicated offscreen RAM at screen size, capped at 1920x1080. Each setting
gets one warmup draw and three timed full-size premultiplied-alpha draws, with
every pixel and the untouched border/pitch padding checked after retirement.
The 16-thread baseline is measured again at the end to reduce clock-warmup bias.
`GPU dispatch: threads=... averageUs=... verified=...` reports each result.
Selection requires at least a 12.5% latency improvement over the better baseline
and any previously selected candidate. Unverified higher settings are rejected.
Allocation failure retains 16 threads. A timeout leaves private probe storage
pinned and selects the existing CPU desktop fallback, avoiding a reset through
an active probe. Normal texture/alpha/scanout readbacks then run at the selected
limit before GPU presentation is enabled. This keeps the previous source-cache,
destination ownership, completion fence and context-retirement requirements.
Compare `selected RCS pixel-shader threads`, desktop `rcsUs`, `armUs` and missed
refreshes under the same visible transparency test. QEMU cannot profile Intel
dispatch or establish a frame-rate improvement on hardware.
The first retained shadow can also use an exact WB palette row for its
background instead of reading WC scanout pixels. This is eligible only before
another shadow or translucent layer changes those pixels; the animated
window's CPU grip rows retain the normal read/modify/write path. Later layers
still read the composed target. `shadowRowFrames` reports frames supplying
the known row. Shadow tests compare both source modes with clipping, occlusion,
padding and offscreen bounds; retained scene tests include the row optimization
across 90 alternating-buffer frames with alpha and overlapping shadows. Compare
desktop `cpuUs` and flip arming time alongside the selected shader limit.
The execution-timing experiment samples every sixteenth compositor loop.
`GPU timestamp clocks:` reports separately calibrated BCS/RCS clock rates
and verified command-store support. Rates are measured twice against TSC;
stationary, implausible or inconsistent counters disable that engine's timing.
Empty offscreen batches verify timestamp stores before runtime sampling starts.
Unmeasured BCS/RCS packets retain their original command streams and completion
checks. Measured RCS packets add five MI_STORE_REGISTER_MEM timestamps around
the existing setup, batch return and drain checkpoints. Measured BCS batches
add a start stamp and a drain/end stamp before the unchanged final fence;
this adds one diagnostic flush and must not be treated as zero overhead.
Timestamp storage is on a separate cache line initialized to a clock value
one second in the past. Stale data, impossible ordering, wrap ambiguities or
large disagreement with CPU elapsed time rejects the sample.
`GPU execution:` records the slowest sampled completed frame in each layout
interval, separately from the slowest `GPU timeline:` frame. Fields are sums
over that frame's measured submissions, in microseconds. `samples=0` is missing
measurement, not zero execution. `invalid` counts rejected samples.
`queueUs` includes publication/context startup until the first GPU stamp.
For BCS, `workUs` is the command interval through the diagnostic work drain.
For RCS, `setupUs` precedes the draw, `parseUs` ends when the draw batch returns
to the command streamer, `rtUs` spans render-target/scoreboard drain, and
`drainUs` spans the remaining full cache drain. Batch return does not by itself
mean all shaders finished. `tailUs` includes the final fence, context retirement
and CPU completion detection after the last GPU stamp. `observedUs` is the CPU
submission-to-detection interval. These are command-stage timings, not a GPU
utilization meter or isolated arithmetic-shader execution time. QEMU verifies
encoding, ordering, timestamp math and fallback; Intel timestamps need hardware.
`GPU timeline:` retains the completed frame with the longest latch interval
within each layout sample. All `composeUs`, `submitUs`, `readyUs`, `armUs`,
`latchUs` and `finishUs` offsets use that frame's loop start. `startGapUs`
measures the time between the previous observed latch and this loop start.
`cpuUs` is scheduled compositor CPU time, while `bcsUs`, `rcsUs` and `sleepUs`
include elapsed waits. `readyUs` is the last CPU observation of a completion
fence, not hardware execution time. `bcsBatches`, `rcsDraws` and `refreshes`
help distinguish submission overhead and missed display refreshes.
`flipPolls`, `maxFlipSleepUs`, `refreshSeenUs` and `surfaceSeenUs` distinguish
repeated observations of a pending display surface from a delayed compositor
wakeup. These are CPU observation timestamps; they cannot identify the exact
hardware latch time while the compositor was asleep. The first two records
describe poll count and longest sleep, while the latter two are loop offsets.
Frames without a confirmed GPU flip break the interval chain, excluding idle
time. A confirmed synchronous flip now supplies the compositor's pacing, so
the next frame starts without a second timer sleep or voluntary yield.
GPU fence/retirement checks and blocking waits remain in place. Idle loops
and CPU presentation keep their existing 60 Hz timer cadence. On hardware,
compare Rainbow alone, Task Manager over it, Explorer over both, and GPU
transparency. Inspect both presentation cadence and CPU use. QEMU cannot
establish Intel timing or guarantee that the hardware meets every refresh.
Rainbow phase uses elapsed time at 240 pixels per second, preserving speed
when presentation changes between 30 and 60 fps. BCS uses a boot-uploaded
720-by-256 immutable tile for regions up to 4096-by-2160, avoiding dependent
scanout self-copies. Larger regions retain the row-doubling path. The offscreen
boot readback crosses the tile boundary on displays tall enough to do so.
Each line counts attempts that stopped at the named first check for that
layout and policy. The last observed value and the expected value share
units (pixels for geometry, inclusive rows for damage, byte addresses for
display surfaces). For row and count bounds, the expected value is the limit.
Move or minimize the overlay
and check for stale pixels, cursor trails and incorrect window edges.
RCS retains four source uploads with separate alpha coverage and GPU addresses.
Unchanged published revisions can be reused across intervening window draws.
Mutable sources still upload on every use. Occlusion fragments of an opaque
scaled client share one RCS submission, up to the existing rectangle limit.
Alpha fragments also share submissions when their combined clipped coverage
fits the rectangle limit. Direct scanout uses fenced WC/UC ownership transfers
after the hidden-buffer CPU-store/blend/readback probe succeeds. The WB master
retains its explicit cache-line flushes.
For the multiwindow test, compare `rcs`, `reused`, `uploadKiB` and
`over2Refresh` at the same window sizes and stack order.
`rcsAlphaKiPx` counts submitted blend pixels
and `alphaSavedKiPx` counts pixels skipped by transparent-band clipping.
Each band can skip its widest interior transparent gap, including gaps
bounded by opaque borders. The isolated readback probe uses that layout.
Compare frame cadence, RCS elapsed time and cache handoff time. A uniformly
translucent surface offers no transparent bands to skip.

The desktop HUD's `Flip` value measures the latest interval between confirmed
flips on consecutive compositor loops. The `GPU present:` record summarizes
those intervals as `avgFlipMs` and `maxFlipMs`. `skippedRefresh` counts display
refreshes between those flips, including the ordinary intervening refresh
at 30 fps on a 60 Hz panel. `over2Refresh` counts intervals spanning more than
two refreshes. Use those values with the existing RCS, handoff, copy and flip
wait totals. The log still captures maximized scenes where the HUD is hidden.

Set `GPU_SCANOUT_COPY_AUTO=1` to compare full-frame BCS and CPU streaming copies
into both hidden scanout buffers at boot. The probe logs the average time for each
path and requires pixel readback and at least a twofold speedup before
selecting CPU streaming. `GPU present:` reports `cpuCopies` alongside copy
time and flip cadence. Compare CPU utilization as well as frame time because
streaming copies occupy the compositor core. BCS is the default for both
builds. The hardware streaming test raised boot CPU from about 6% to 22%
and still showed roughly 50 ms between flips under RCS translucency.

With a mock GPU image built using `GPU_COMPOSITOR_TEST`, `GPU_PRESENT_TEST` and
`GPU_RENDER_TEST`, add either measurement flag and run
`python3 tests/qemu_gpu_perf_test.py` to verify matching sample
windows, phase counters, zero mock GPU sleeps and bounded output. Mock phase
timings do not represent Intel execution or vblank behavior.
Build with both flags off and pass `--quiet` to check that periodic reports
stay out of Boot Log.

`make test-alpha-renderer` checks SIMD source-over against the scalar reference
with exhaustive alpha/channel values and randomized scaling, clipping,
transparent/opaque/malformed pixels and guarded row padding. The host timing
printed at the end is informational and is not an Intel GPU comparison.
The compositor suite explicitly disables CPU alpha to test RCS/failure
paths, then enables CPU alpha and compares pixels with RCS. It also
places a 900x650 transparent/half-alpha/opaque layer over fullscreen animated
Rainbow, checks changing pixels for several frames, and requires zero RCS
submissions for that bounded layer with `GPU_RCS_ALPHA=0`.
After building production, run `python3 tests/qemu_simd_test.py` for SIMD
preservation across syscalls/timer preemption. On hardware compare `alphaMs`,
RCS/handoff totals, CPU use and visible motion. There is no default size-triggered
switch to RCS alpha. `python3 tests/qemu_gpu_compositor_test.py --large-alpha`
uses a 1920x1080 desktop and resizes an alpha layer through 900x650, 1300x900
and 1800x950 over fullscreen Rainbow. It checks animated pixels, zero RCS
submissions and intact heap/stack guards above the former cutoff. This tests
the path transition, not physical frame rate. Measure CPU use for large layers.

For `GPU_RCS_ALPHA=1` direct alpha draws covering at least 65,536
pixels, completion waits can use a short adaptive poll near the GPU drain.
Two valid timestamp samples with matching source/sampling/rectangle geometry
are required. All eligible draws share a 250-us polling allowance per compositor
frame; ordinary one-ms sleeps resume when the prediction misses or the allowance
is spent. IRQs stay enabled and both fence completion and context retirement
remain mandatory. Missing/invalid timing disables learning; timeout still takes
the existing CPU recovery path.
`GPU alpha wait` reports `windows` (waits using the poll), `pollUs` (total elapsed
poll time, including any interrupt/preemption delay), and `predictionUs` (the last
GPU-to-drain estimate). Allowance accounting can overshoot by the final poll/check
or an interruption. Confirm nonzero `windows` before attributing hardware gains.
Compare fullscreen transparency over Rainbow, CPU use, RCS drain-to-observation
tail and refresh skips; keep Start close in the sequence. QEMU does not execute
Intel CSB polling. `make test-gpu-alpha-wait` uses a fake timer/GPU completion
timeline to check warmup/invalidation, shared budget exhaustion, wrong predictions,
and missing-fence/missing-retirement timeouts. Its simulated latency improvement
is not a hardware FPS measurement.

`make test-title-bar test-damage-regions` verifies cache invalidation keys,
text/icon/control pixels, offscreen clipped blits and direct fallback, plus
non-inflating exposed-strip coalescing and bounded damage coverage under pressure.
The compositor suite checks cache reuse, title/focus/resource invalidation,
and shared scene repair batching. Its mock retained destination checks that
overlapping damage becomes disjoint clips, holes remain untouched, and multiple
clips submit RCS once per window in stack order. HUD/taskbar damage is separated
from interior repairs. An injected partial repair failure must reconstruct the
scene through CPU fallback without accumulating alpha. This fixture invokes the
shared repair traversal directly; it does not validate Intel scanout registers.
The experimental `GPU repair plan` log reports `sceneBatches` and `sceneRects`;
compare these with frame-gap `bcsBatches`, `rcsDraws`, `repairsUs` and `fallbackUs`.
With exact retained damage, clock/FPS updates keep their horizontal extent and
redraw only the opaque tray. `trayRects` counts those repairs and `trayUs` is
their total elapsed time in the sample window. Full taskbar drawing shares the
same tray renderer; ordinary taskbar changes and CPU fallback remain supported.
The tray path also works under a cached Start panel that ends above the taskbar.
When repair-time revalidation discovers a changed animation layout, one bounded
rebase can repair the full screen's complement of the fresh animation plan and
then draw that animation once. Revalidation runs again; another change, overflow
or GPU failure retains full-scene recovery. `rebases` counts attempts and
`rebaseFailed` counts attempts rejected by the second revalidation. These two
counters are cumulative so a layout-sample reset cannot hide an attempt. Host
pixel comparisons cover complementary repairs across menu changes, moves, source
coverage changes and reorder on both scanouts. Physical transition latency
still needs a new log; QEMU cannot reproduce the Intel mid-frame timing.
The QEMU suite compares full and partial tray repairs with full taskbar pixels,
including changing digit widths, clipped text/borders, untouched task buttons,
and zero extra GPU submissions. Use a new Kaby Lake log to verify that removing
the former full-taskbar repaint from clock/FPS frames recovers refresh headroom.
On Kaby Lake, test fullscreen Rainbow with the transparent GPU Scaling Test
maximized above it, then Image Viewer launch, Start open/hover/close, and window
restore/resize/close. Keep Start close in the sequence even if steady animation
looks smooth. Record FPS and save the hardware boot log; QEMU cannot establish
whether the 50 ms frames or visible fullscreen transparency drops improved.

The compositor suite also checks title hover damage, cache reuse after moves,
and release on real app close. Its partial-alpha
fixture requires exactly two one-pixel RCS draws and preserves an unrelated
sentinel on the same scanline. Ordered draws subtract higher opaque coverage
without discarding backgrounds beneath alpha/shadows. Measure the new covered-
Rainbow CPU/frame costs separately on hardware; QEMU is not a GPU benchmark.

`make test-window-shadow` checks cached edge/corner mask pixels against an
independent distance/falloff reference, negative positions, clipping, padded
rows and cache reuse after resize. Shadows are rendered by an isolated optimized
CPU object independently of kernel `OPT`. The compositor integration suite
enables fixture shadows explicitly and checks layering over an RCS client,
wallpaper shadows, repeated one-pixel damage, CPU/GPU agreement, retained interior
updates, and shadow removal on maximize/hide. It also covers Rainbow animation
beneath a translucent client and requires BCS palette submissions rather than
CPU full-surface generation. On hardware, compare CPU usage and frame smoothness
with alpha off/on; the owner's previous build rose from about 5% to 24% CPU.

Client alpha (ABI 11) is opt-in through syscall 223. Launch
`run /bin/gpu_test.elf` and press A to toggle transparent, half-alpha and opaque
bands. On hardware, verify wallpaper/animated windows remain visible beneath
the first two bands, then resize, overlap, minimize and restore. Compare with
`GPU_RENDER=0` for the CPU blend path. Title bars and grip marks stay opaque;
transparent pixels still receive pointer input.

`make test-gpu-render` checks every alpha/destination byte pair, alpha planner
bounds and exclusion of translucent scenes from deferred opaque batches.
The mock QEMU compositor suite checks overlapping translucent clients, scaled
background pixels, CPU grip rows, repeated one-pixel damage without opacity
accumulation, CPU/RCS output agreement and partial-blend failure recovery.
The standalone window suite exercises the real owner-only alpha syscall via
the app's A toggle. These checks do not execute Intel blending hardware.

For the render-engine state/context regression, run `make test-gpu-render`.
The ASan/UBSan test decodes Gen9 texture/alpha fields, checks clipped texture
coordinates and sentinels, validates logical-context register slots, and checks
foreground subtraction for scaled clients. It does not execute GPU shaders.
Build with `make iso` for the default physical offscreen RCS readback gate.
`GPU_RENDER=0` selects blitter composition and `GPU=0` selects CPU rendering.
`GPU_TRACE=1` adds extended boot stress sweeps; normal builds use short checks.
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
The restored revision subsequently froze on physical enlargement even with all
other windows minimized. The current diagnostic builder uses full-client
geometry and hardware scissor. Host tests require identical vertex bytes across
damage clips, check signed offscreen positions and one-pixel/inclusive edges,
and the QEMU backend decodes the actual encoded vertices/scissor for its pixels.
The boot readback gate must pass clipped copy/scaling/alpha and border checks;
passing host/QEMU tests does not confirm that physical enlargement is fixed.
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
make iso CXX='clang++ -DGPU_COMPOSITOR_TEST -DGPU_PRESENT_TEST -DGPU_RENDER_TEST' \
  GPU_RCS_ALPHA=0 GPU_ALPHA_REPAIR_BATCH=1 GPU_EXACT_REPAIRS=1 GPU_RETAINED_MENU=1 \
  PERSIST_DISK=.storage-test/gpu-test.img ROOT_MIB=64
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
| `python3 tests/qemu_usb_probe_failure_test.py` | Timer and desktop progress during stalled synchronous discovery, bounded hub cleanup and existing input/disk survival; uses a disposable copy of `.storage-test/root.img` with the current ISO |
| `python3 tests/qemu_usb_recovery_test.py` | Root-failure report and topology diagnostics |
| `python3 tests/qemu_font_selection_test.py wyzar-usb.img assets/fonts/selawik/selawk.ttf` | Custom/missing/corrupt TTF selection and recovery font |

Other pointer/drag/RTC fixtures may exist locally but are not all tracked. Do not
make them prerequisites for a fresh checkout. See [USB_INPUT](USB_INPUT.md) and
[FONTS](FONTS.md) for the corresponding implementation boundaries.

## Physical evidence

`make test-physical-pages` runs the actual physical allocator with a host lock
standing in for interrupt exclusion. Eight callers check unique ownership,
zeroing, recycling and region-gap preservation. An unguarded negative control
fails with duplicate page ownership. This validates allocator exclusion, not an
Intel crash reproduction.

With the mock GPU image, run `python3 tests/qemu_compositor_stress_test.py` to
launch real GPU tests, Notepad, Explorer and Task Manager over animated alpha
and Rainbow fixtures. It inspects every heap header and task stack canary.
The compositor integration test also checks the heap and compositor stack
headroom after native resize/close/retirement. Intel DMA remains untested here.

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
