# Pending platform work

Updated 2026-11-11. Implemented behavior belongs in the feature guides linked
from [README](../README.md); this file tracks remaining work.

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

1. Safe eject and stable volume identity; explicit reconnect/recovery policy.
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
validate user pointers and revoke mappings on teardown. Complete address-space
cleanup and a shared userspace syscall/window library.

Package Counter and future apps such as Notepad with their own manifest/icon
assets. Add file transfer/management and a standalone image viewer. A versioned,
self-contained `.wiz` package and package installer remain possible additions;
current `.wiz` files are ordinary WyzarOS ELFs. See [APP_LOADING](APP_LOADING.md)
and the [Window API](../APIDocs/WindowAPI.md).

## Desktop and display

Add a window-alpha contract, shadows/retained decorations and optional client
double buffering. Runtime wallpaper/font selection can build on boot-time file
settings. PNG icon alpha does not require a GPU.

Measure tearing/flicker before hardware cursor/page-flip work. Keep Intel GPU
bring-up opt-in until rendering, memory management and recovery are ready.
Follow the dependency order in the [display roadmap](GPU_DESKTOP_ROADMAP.md).
