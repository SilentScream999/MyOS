# Pending platform work

Updated 2026-11-24. Implemented behavior belongs in the feature guides linked
from [README](../README.md); this file tracks remaining work.

## GPU follow-up

The retained Rainbow build reached 58–59 FPS with the grouped app picker
open and 58–60 FPS with tall half-screen translucency on the Kaby Lake
laptop. Some layout transitions still missed refreshes. If GPU work resumes,
use the build recipe in [BUILD_CONFIGS](../BUILD_CONFIGS.txt) and include Start
close, picker open/close, and moving or resizing a tall translucent window
over Rainbow. The default build uses CPU SIMD translucency.

The scheduler currently runs on one core. Multicore work needs its own
scheduler, interrupt routing, and shared-state synchronization validation.


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
