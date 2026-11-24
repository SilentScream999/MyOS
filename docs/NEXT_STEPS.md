# Pending platform work

Updated 2026-11-22. Implemented behavior belongs in the feature guides linked
from [README](../README.md); this file tracks remaining work.

## Current GPU stability gate (2026-11-22)

Retest opening apps while Rainbow and animated transparency run. The owner
photograph shows heap corruption during `kmalloc scan`; the accompanying log
predates that boot. Physical page allocation now serializes reservation/recycling
against preemption, and the compositor has a larger stack with frame snapshots
stored statically. Stack canaries and richer heap diagnostics are enabled.
The photographed crash cause is still unconfirmed. Covered-window CPU use and
alpha latency remain open; zero-upload timing samples still show large GPU
wait/presentation costs. Validate stability first, then investigate those waits.
The next measurement build separates scanout copying, cursor/outline drawing
and flip-latch waiting, and records actual one-ms GPU sleep return durations.
Compare Rainbow alone, an opaque covering window, and small/enlarged alpha;
hold each layout for five seconds and save the three adjacent GPU log records.
The follow-up owner run narrows the priority to alpha: the default small test
raises CPU from about 5% to 9% even away from Rainbow; opaque overlap/shadows
are now acceptable. All logged GPU sleep returns are below 2 ms, uploads are
reused, and cursor/outline drawing is negligible. The slowest sample records
40 loops/2016 ms with 43 RCS draws, 426 ms RCS elapsed and 115 ms handoffs
(about 9.9 and 2.7 ms/draw respectively). Reduce translucent rendering and
ownership-handoff work; do not treat these timings as a measured CPU breakdown
or remove cache/fence/retirement checks based on this log.
The new revision blends alpha through an optimized SSE2 CPU path at all supported
sizes, retaining BCS underneath and avoiding RCS handoffs for translucent layers.
The owner confirms smoothness until the previous size cutoff selected RCS; that
default cutoff is now removed. Validate resizing across the former cutoff:
`cpuAlpha`/`alphaKiPx` should increase while that layer's RCS and handoff costs
disappear. Measure CPU use as well as motion, particularly for large layers.
Opaque scaling retains RCS; no GPU shader/cache-policy change is claimed.

## Verify the combined build on hardware

Retest on the Intel Gen9 laptop and Surface/USB-C setup:

- Boot/root selection, late discovery and first-boot capacity setup.
- Keyboard, mouse/held dragging and wireless input during storage activity.
- Create, atomically replace, append, rename and reread files across boots.
- Start-menu launch, close requests, Ctrl+C and terminal restoration.
- Lazy PNG icons, PNG/JPEG wallpapers, selected TTF fonts and display behavior.
- Shutdown/restart with the boot disk present and after removal.

Use a spare stick and retain saved files before reflashing. Keep physical results
distinct from the reproducible host/QEMU checks in [TESTING](TESTING.md).

## Complete storage semantics

The requested storage work already includes batched writes, responsive runtime
waits, atomic saves, rename and automatic capacity setup. Remaining follow-up:

1. Stable volume identity and an explicit reconnect/recovery policy. External FAT32/exFAT eject is implemented.
2. Permissions/user identity, link semantics and a consistent UTC timestamp basis.
3. Reclaimable vnode caches and broader, deliberately validated ext4 profiles.
4. More asynchronous discovery/control probes; current waits are bounded but
   can still delay input during enumeration.
5. GPT and wider block commands, then NVMe/AHCI and an installer. USB remains a
   permanent system-drive target using the same block/VFS interfaces.

Do not weaken journal barriers or silently redirect failed saves into RAM.
Reclaiming the first-boot disk seed and live/in-place resizing are separate from
the implemented replacement provisioning. See [EXT4](EXT4.md).

## Runtime clock settings

The tray uses the RTC with a build-time fixed offset; there is no named
timezone/DST, runtime setting or network-time service. Next:

1. Define RTC interpretation (local or UTC) separately from display zone.
   Preserve dual-boot compatibility without rewriting the RTC twice.
2. Persist runtime settings: fixed offset first, then named zones/DST.
3. Provide a UTC-based service/syscall for filesystem timestamps. Keep monotonic
   scheduling/uptime independent of civil time.

Acceptance: changing settings updates the tray without recompiling, survives
reboot, handles date rollover and gives new file timestamps a defined UTC basis.

## Applications and API hardening

Validate ELF tables/segments, honor permissions, enforce window ownership,
validate user pointers and revoke live window mappings on teardown. Harden
loader failure cleanup and add a shared userspace syscall/window library.

The `.wiz` package manager, native File Explorer and Notepad are implemented.
Next app work includes Unicode text, terminal output selection, configurable file associations, recursive folder copying,
cross-filesystem moves and trash/undo. See [Explorer](EXPLORER.md),
[packages](APP_PACKAGES.md) and the [Window API](../APIDocs/WindowAPI.md).

## Desktop and display

The owner confirms the current Gen9 composition, page flips and RCS scaling
function without failures. These paths now probe by default with runtime
readback gates and CPU fallback.

Opt-in premultiplied client alpha is implemented (ABI 11, syscall 223), including
ordered RCS blending and CPU fallback. The owner confirms it works without
failures, but observes Rainbow slowing and CPU rising from about 5% to 24%.
Ordered scenes now retain BCS palette/opaque rendering. The GPU Scaling Test
toggles alpha with A. Measure the new CPU/frame costs on hardware.

The owner confirms cached shadows work without failures. Partially covered
Rainbow still runs slower/more CPU intensive than fullscreen, especially with
transparency. Title bars now cache text/icons/controls. Ordered client damage
uses its actual bounds, skips higher opaque coverage and preserves exposed-strip
damage instead of expanding it into hidden bounding-box pixels. Validate title
focus/hover/resize/resource changes and compare covered-Rainbow frame/CPU costs
on hardware. Host and mock QEMU tests do not validate Intel execution. Next add
optional client double buffering/present acknowledgments.

The latest laptop comparison still reports ~5% Rainbow, ~12%+ under a small
opaque test, ~2% under a large opaque test, and ~12% plus lag after A. RCS now
reuses unchanged immutable uploads; the remaining cost needs measurement.
Hold each layout for five seconds and inspect the bounded `GPU perf:` Boot Log
samples. Destination cache handoffs remain conservative; do not remove them
based on host/QEMU output alone.
The follow-up log confirms upload reuse and large BCS/presentation wait totals.
Compositor one-ms sleeps now wake at their timer deadlines rather than waiting
for a normal ten-ms scheduling quantum. Validate the change on the laptop;
GPU cache/fence/display synchronization is unchanged.
The next log still reaches F ~50 ms beneath alpha, with zero uploads and many
BCS submissions per loop. GPU Rainbow exposed regions now share one scene pass;
ordered alpha/shadow scenes avoid repeated HUD-band composition. Check the new
submission counts, F and CPU on hardware. CPU mode retains exposed rectangles.
Hardware cursor, interrupt completion, general GPU allocation and idle power
management remain pending. Runtime wallpaper/font selection and PNG icon alpha
already work independently of client alpha.
Follow the dependency order in the [display roadmap](GPU_DESKTOP_ROADMAP.md).
