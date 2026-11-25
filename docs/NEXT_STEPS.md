# Pending platform work

Updated 2026-11-25. Implemented behavior belongs in the feature guides linked
from [README](../README.md); this file tracks remaining work.

## Audio

The initial shared HDA driver exposes analog route diagnostics and short stereo
test tones. ThinkPad E470 headphones and speakers are physically confirmed;
`audio unmute` restored speakers by clearing EC byte `0x30` bit 6 (`0x40` to
`0x00`). See [the audio guide](AUDIO.md). Physical multimedia-key delivery
is confirmed for plain F1/F2/F3 and, after firmware setup, Fn+F1/F2/F3.
The captured firmware now has narrowly fingerprinted SAUM(2)/UCMS(0x1c) setup;
the fourth hardware capture confirms SCI_EN=1 and multimedia make/break codes.
Fn+Esc changes the row mapping as expected. General ACPI HKEY/GPE event
handling still requires wider ACPI support.
The new `play /path/song.mp3` foreground player decodes in userspace and streams
native 44.1/48 kHz stereo PCM (windowed-sinc fallback resampling); E470 playback
opens the firmware mute gate automatically.
Shared mute/volume support multimedia PS/2 keys; the temporary plain F1/F2/F3
fallback is removed now that firmware media delivery works. The owner confirms good sound/no errors after the dedicated audio
task, 256 KiB ring and native-rate/improved resampling changes. Retest the new
volume hold-repeat and clock-adjacent slider/mute popup on E470, including the
physical GPU retained-popup path.
The owner reports Rainbow lag on GPU while changing volume, even with Boot Log
covered. Retest the memory-only control logging and bounded retained popup/tray
repair fixes under that exact layout; GPU mock validates covered-log behavior
and cheap repair acceptance, not real frame timing.
The owner reports that the GPU volume fix seems fine and Fn volume works after
starting `audio hotkeys`. That exposed lazy initialization, now changed to a
background audio probe after the first desktop frame. Verify Fn keys immediately
after a fresh E470 boot without running any audio command first, including
hold/release and popup state. The startup change is QEMU tested; firmware key
delivery after initialization is already confirmed by capture and owner.
The profile remains specific to TP-R0D/revision 0x960; IRQ9 stays masked and
pending EC events remain unconsumed until wider ACPI dispatch support exists.
Next features: USB HID consumer-control media keys, WAV, seeking/playlists, live jack routing,
mixing and a graphical player. Volume settings currently reset on reboot.

## GPU follow-up

The retained Rainbow build reached 58–59 FPS with the grouped app picker
open and 58–60 FPS with tall half-screen translucency on the Kaby Lake
laptop. Some layout transitions still missed refreshes. If GPU work resumes,
use the build recipe in [BUILD_CONFIGS](../BUILD_CONFIGS.txt) and include Start
close, picker open/close, and moving or resizing a tall translucent window
over Rainbow. The default build enables the retained path on validated hardware.

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
