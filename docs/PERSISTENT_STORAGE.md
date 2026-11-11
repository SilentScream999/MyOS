# Persistent system storage

Updated 2026-11-10. New USB images contain a FAT EFI boot partition and a journaled
ext4 system partition. System roots are ext4-only; the kernel FAT16 backend and
legacy creation/migration tooling have been removed. See
[EXT4.md](EXT4.md) for the feature profile and verification.
The system partition mounts at `/`; applications do not
use a `/disk` prefix. This is a USB system root, not an internal-drive installer.

## Build and boot

From WSL in the repository:

```sh
make usb-image       # refresh boot files AND bundled disk programs; preserve user files
make run-usb         # build and boot the disk image directly in QEMU
make iso             # build recovery/live ISO and refresh the USB image
make run             # boot ISO in QEMU, with the USB system image attached
```

Use `wyzar-usb.img` for physical USB testing. The standard image now uses ext4
for its system partition. Write the whole image with your usual
imaging program. Reflashing the whole image replaces the physical stick's saved
files, even though refreshing the host image preserves its saved files. An
in-place updater for a running/physical installation is not implemented.

`PERSIST_DISK=another.img` selects a different host image. `make clean` leaves disk
images alone. Close QEMU before updating its image; the updater locks the image
and atomically publishes a completed replacement. It never opens physical drives.
A failed update leaves the original image intact. The updater accepts only this
project's recognized image layout and validates/checks ext4 before editing it.

`FRESH=1` intentionally resets all saved files, settings, applications and wallpaper:

```sh
make usb-image FRESH=1
```

To create/reset a different root capacity, pass `ROOT_MIB`, for example
`make usb-image FRESH=1 ROOT_MIB=1024`. Ordinary updates retain capacity;
fresh resets retain a recognized existing capacity unless explicitly overridden.
To grow an existing host image while preserving its files, use
`python3 tools/expand_usb_image.py wyzar-usb.img --root-mib 1024`.
`--total-bytes` instead fits the complete image to a stick's actual byte capacity.
Expansion is offline and atomic; it does not access the stick or resize a live
root. Reflash afterward. Windows-backed images are dense and require space for
the full output; native Linux images can use sparse storage. Details and bounds
are in [EXT4.md](EXT4.md).

## Layout and root selection

| Partition | Size | Purpose |
|---|---|---|
| WYZARBOOT | 128 MiB | EFI boot, Limine, kernel, small recovery initrd and init module |
| WYZARDATA | 256 MiB default; configurable | ext4 persistent system root `/` |

Normal refresh preserves the ext4 data volume. Older FAT-root images are rejected;
`FRESH=1` replaces them with the current layout. The boot partition remains FAT16
for firmware compatibility and is not mounted by the kernel.

| Path | Backing and purpose |
|---|---|
| `/bin` | Disk programs; shell/Start menu discover programs here |
| `/etc` | Disk settings and system-root signature |
| `/home` | Persistent documents and personal assets |
| `/share` | Persistent bundled assets |
| `/tmp`, `/run` | RAM mounts, cleared at boot |
| `/bootlog.txt`, `/run/boot.log` | Live RAM kernel log views; stat/listing size follows current log length |
| `/dev`, `/mnt` | Reserved directories for future devices/additional mounts |

The kernel initially unpacks recovery programs into RAMFS. After USB enumeration,
exactly one matching WYZARDATA disk must be available. `/etc/root.cfg` must begin
with `WYZARROOT1` plus a newline, and `/bin/init.elf` must exist. Runtime/log mounts
must attach successfully before disk root is accepted. The shell then starts from
disk. Missing, invalid or ambiguous storage selects RAM recovery; failure to load
the disk shell also selects the full recovery tree. Recovery has a procedural
wallpaper and nonpersistent files. The boot log identifies the selected mode.

Selection happens once before userspace starts. Storage unplug/controller failure
offlines the mounted filesystem until reboot; failed saves are never silently
redirected to RAM. Selection by stable boot-disk identity remains future work.
The general mount table supports up to 16 mounts; path traversal handles `.` and
`..` across mount boundaries, and mounted files cannot be unlinked through VFS.

## Updates and ownership

Normal image refresh replaces these OS-owned paths from the current build:

- `/bin/init.elf`
- `/bin/counter.elf`
- `/bin/win_test.elf`
- `/etc/root.cfg`
- `/share/fonts/selawik.txt` (bundled font license)

Other applications, documents, configuration and existing wallpapers remain.
Missing default directories/settings/wallpaper are seeded. A full directory/disk,
conflicting system path or malformed FAT causes the update to fail, leaving the
original image intact. Use a different executable name for a user replacement of
a bundled application; bundled names are owned by the OS updater.

## Wallpaper

`/etc/desktop.cfg` contains a single absolute file path, defaulting to:

```text
/share/wall.raw
```

The initial asset is copied from the repository's `wallpaper.raw` during image
creation/migration only when `/share/wall.raw` is absent. Later builds preserve an
existing disk wallpaper. To restore the repository asset, remove the disk default
before refreshing the image, or intentionally create a fresh image.

Settings changes apply on reboot. Use 8.3-compatible disk names, such as
`/home/blue.raw`. WRAW is a 16-byte little-endian header (`WRAW`, uint32 width,
uint32 height, uint32 format=0), followed by BGRA8888 pixels. Legacy headerless
BGRA is accepted only at exactly the current framebuffer dimensions.

Boot keeps the animated splash visible through storage discovery and the first
wallpaper attempt. After the system root mounts, the selected file is read once
(maximum 64 MiB, 15-second read budget checked between chunks), checked for a complete read and
validated/scaled into a separate RAM buffer. Only successful decoding replaces
the current wallpaper. Missing/corrupt/unreadable/timed-out assets keep the procedural
fallback. The compositor presents its first complete desktop only after this attempt
finishes, so successful boots do not expose a temporary blue desktop.
Native-size wallpaper conversion uses four-pixel SSE2 copies with opaque alpha;
other sizes use exact nearest-neighbor scaling without per-pixel division and
reuse repeated source rows. Boot generates the procedural fallback only when
the disk wallpaper is unavailable, rather than generating and replacing it on
every successful boot. Invalid assets still select that fallback before desktop
presentation. Optional `BOOT_DIAGNOSTICS=1` builds separate UI initialization and
font load/bake time from wallpaper I/O and conversion, and record the asset's
byte count. These extra timing/size lines are off in normal builds; failure
messages remain.
The compositor reads RAM on every frame; it never reads the disk. The recovery
initrd and Limine module list contain no duplicate wallpaper assets.

Reads use 64 KiB filesystem chunks to permit preemption between synchronous
operations. The ext4 block adapter batches reads into up to 128 sectors.
The USB driver issues READ(10) commands for up to
128 sectors (64 KiB). Its DMA buffer is a physically contiguous, 64 KiB-aligned
subrange of a retained 128 KiB allocation, so a transfer cannot cross xHCI's
64 KiB buffer boundary. Other block backends can omit the optional
multi-sector callback and retain one-sector reads. Bounds are checked at the block,
partition and READ/WRITE(10) boundaries. Writes now batch up to 128 sectors
(64 KiB) per USB command too. Every completed filesystem block write retains
its device flush; repeated flush requests without new writes are skipped.

Large assets can still slow boot; this is
not an asynchronous I/O implementation. In diagnostic builds, wallpaper duration is logged in
milliseconds for hardware diagnosis, split into disk read and decode/scale.
USB discovery, disk readiness/capacity, initial desktop composition and total
kernel-to-desktop time are also logged. The latter excludes firmware/bootloader
time. Physical-device performance still needs verification.

Root ports are powered together and given one shared 100 ms settling window per
controller. Previously every empty port added its own 100 ms wait. Per-device
port-reset and descriptor/readiness handling are retained.

## Intermittent cold-start failures

The SCSI readiness probe now allows up to five seconds of polling for recognized
transient readiness responses, instead of four quick attempts. Ready devices
continue immediately. READ CAPACITY has a two-second retry budget and reads a
one-second retry budget, checked between complete commands. A 32-attempt cap also
bounds persistent reset attention. Individual USB transfer timeouts can extend
elapsed time beyond those between-command budgets.

Retries use REQUEST SENSE and accept current reset attention or recognized
becoming-ready/operation-in-progress conditions. Media-change attention is
accepted during initialization only; media-change on a read offlines the mounted
disk until reboot. Permanent errors, malformed/deferred sense and transport
failures are not blindly retried. Writes and flushes are never replayed. This is
not USB BOT reset/stall recovery and does not fix failures before device discovery.

If wallpaper or disk startup fails, the messages remain in the RAM kernel log
after the desktop appears. Run `open bootlog` or `cat /bootlog.txt`; photograph
or record the storage/sense/USB lines before rebooting. The log distinguishes
readiness/capacity failure, no readable system partition, failed root validation,
USB transport errors and short wallpaper reads. Physical cold-start behavior must
still be retested; the original failed attempt's log was not captured.

## Shell and current limits

When boot selects RAM recovery, `cat /run/storage.txt` shows a small RAM-only
snapshot of the selection failure and USB topology. It distinguishes absent or
offline disks, unrecognized partitions, ext4 mount/journal failure, root-marker
or shell-file failure, and runtime-mount failure. It does not write to the USB
device or print extra success-path timing logs. This file is created on recovery,
not on normal disk-root boots.

Before root selection, if no USB disk is online, boot allows a 1.5-second
discovery window for late root/hub connections. Existing enumeration timeouts
still apply and may extend this window while a probe completes. Boots with an
already-online disk skip the wait. Root selection remains fixed after userspace
starts. The recovery snapshot includes controller state and connected/attempted
root-port PORTSC values, alongside hub and disk details.

`python3 tests/qemu_late_storage_test.py` verifies disk arrival during this window
and confirms that hotplug after RAM recovery does not replace the running root.

Shutdown/restart attempt to close the ext4 journal first. A removed or failed
disk cannot be flushed, so a close error is reported and the requested power
action continues. Removing the boot drive does not permit a clean unmount or
save outstanding writes. `python3 tests/qemu_power_action_test.py` checks both
power actions with a connected disk and after unplugging it through a hub,
including filesystem recovery and previously saved file contents.

```text
ls /
ls /bin
write /home/hello.txt hello-from-wyzar
cat /home/hello.txt
stat /home/hello.txt
rm /home/hello.txt
open bootlog
```

The shell's `write` saves via atomic replacement, so shorter replacements do not
leave trailing bytes and an interrupted save retains complete old/new contents.
`append <path> <text>` adds to the end, and `mv <old> <new>` renames when the
destination is absent. Open files cannot be renamed/replaced. Atomic-save cleanup
can leave a `.wy-save-N` sibling after interruption. Ordinary append and direct
`O_TRUNC` writes are not atomic saves. Runtime wallpaper reload is not implemented.

Disk filenames are case-sensitive, with up to 255 bytes per component and growing
directories. Read/write/create/unlink/stat/mkdir/rmdir/readdir and seek are
implemented. `O_TRUNC`, descriptor truncation (syscall 77), and sync (syscall 74)
are available for ext4 and RAMFS. Growing with truncate zero-fills the extension;
it allocates/writes data rather than creating a sparse hole. Rename (syscall 82,
no overwrite) and atomic replacement (custom syscall 138) are available too.
`rm` refuses directories and open files. Civil timestamps remain unknown until
the OS has a UTC timestamp service. Filesystem calls remain synchronous for the
caller, but runtime USB completion waits sleep under a reentrant filesystem
mutex. Timer/input processing can continue. Initial probing still uses bounded
synchronous control requests. BOT transport recovery remains future work; a
failed transport is offlined. Successful durable writes require the device to
honor SCSI SYNCHRONIZE CACHE. ext4 journaling supports metadata recovery; it does
not guarantee preservation of every interrupted application write.

Only USB BOT/SCSI LUN 0, 512-byte sectors, READ(10), MBR and the restricted
ext4 system profile are supported. No UAS/GPT/FAT32/exFAT/internal SATA/NVMe
support exists.
Root-port status notifications are acknowledged without resetting xHCI; a
CCS (connection) change must stay stable for 150 ms before re-enumeration.
CSC flags are retained before acknowledgment, so unplug/replug between samples
also triggers local replacement even if final CCS is still connected. PED
changes alone never trigger a controller reset: late enablement after a port
reset timeout otherwise causes a repeating reset loop. Root connections now
enumerate/remove only that root port, preserving live input queues and disks on
the same controller's other ports. Root resets wait up to 500 ms for both reset
completion and enablement; a timed-out, unbound port that becomes ready later
gets one local enumeration attempt. Failed descriptor enumeration is retried on
a subsequent physical disconnect/reconnect, rather than in a reset loop.
Removing a disk (or its upstream root hub) offlines only disks on that root port;
old mounted handles are never rebound on reconnect. Root-port fallback scans run
at the input task's 8 ms cadence. A 50 ms scan experiment showed no measurable
hardware CPU benefit and was reverted. Devices/DMA buffers are
retained after removal to keep late completions safe, so hotplug is not yet
allocation-free. Hub notifications read
and clear latched changes before rearming their interrupt pipe; only stable
connection changes request re-enumeration. Downstream hub-port changes now detach
and enumerate only that child branch, retaining sibling input and disk objects.
Parent links track nested hub descendants, and latched connection changes handle
rapid replacement. Interrupt bitmaps select affected ports for immediate checks;
debounce checks run every 50 ms. The idle fallback audits one rotating port every
250 ms and hub-level status once per sweep, rather than scanning every port each
time. A missed interrupt is detected within roughly two seconds on an eight-port
hub (four seconds at the supported sixteen-port maximum), plus debounce and USB
request time. Deadlines start after requests finish to prevent busy catch-up
polling. Background GET_STATUS and CLEAR_FEATURE requests are asynchronous: each
hub owns persistent DMA storage and one outstanding EP0 request. The normal event
dispatcher matches controller/slot/endpoint/TRB addresses and copies completion
fields; it never retains an event-ring pointer. The input task continues processing
other devices and sleeping at its 8 ms cadence while requests complete. Hub flags
are acknowledged before rearming the notification pipe, and notifications arriving
during a scan remain pending. A 250 ms completion timeout, error or short status
read stops that hub's background monitor; it does not reset the controller or
offline sibling disks. Reconnecting the hub recreates its monitor. Timed-out EP0
requests are not retried without endpoint recovery; their allocations remain alive
for late DMA. Initial enumeration and actual hotplug enumeration still use bounded
synchronous control requests. SuperSpeed EP0 sizes are decoded as exponents.
USB 2.0 hubs and 5 Gbit/s SuperSpeed hubs support downstream enumeration with
nested route contexts. SuperSpeed uses descriptor 0x2a, SET_HUB_DEPTH, 512-byte
EP0 packets, and endpoint companion burst/interval fields. Its port power bit
must not be decoded as USB 2 low speed. Both paths preserve registered storage
devices. Hub ports are powered together and given the descriptor's power-good
delay (at least 100 ms) before connection/reset handling. SuperSpeedPlus hubs
now use the xHCI compatibility path and controller-reported output speed, rather
than being blanket-rejected. Nonstandard speed IDs remain unsupported. See
[USB_INPUT.md](USB_INPUT.md) for scope and tests. Physical USB-C/Surface behavior
still needs retesting.

Hub/HID enumeration has a shared three-second probe deadline, including nested
hubs; individual command and controller-state waits are bounded. Disk readiness
retains its separate bounded cold-start policy. Failed hub probes get a separate
one-second best-effort slot cleanup; a failing connection can briefly pause
synchronous enumeration but cannot wait forever. Failed controllers are parked,
not reset indefinitely. Legacy handoff disables firmware SMI enables and follows
the relative extended-capability chain. Cold-attach root ports receive a warm
reset even if CCS has not appeared; SS inactive/compliance links use warm
recovery too. A USB-C connector alone does not establish which logical hub/speed
path is used.

## Verification

Tracked ext4 and USB regression checks:

```sh
python3 tools/make_usb_image.py --output .storage-test/ext4-usb.img --fresh
python3 tests/run_ext4_tests.py
python3 tests/ext4_image_test.py
python3 tests/atomic_save_test.py
python3 tests/capacity_image_test.py
python3 tests/run_usb_hub_tests.py
python3 tests/qemu_ext4_test.py --hub-depth 2
python3 tests/qemu_late_storage_test.py
python3 tests/qemu_usb_recovery_test.py
python3 tests/qemu_power_action_test.py
python3 tests/qemu_storage_api_test.py
python3 tests/qemu_storage_async_test.py
python3 tests/qemu_font_selection_test.py wyzar-usb.img assets/fonts/selawik/selawk.ttf
```

The VFS tests cover long names, directory growth, cross-block I/O, failure
containment and journal recovery with sanitizers and Linux filesystem checks.
Image tests verify fresh creation, preserved user files/settings, unsupported
features and legacy-root rejection, and atomic publication on failed updates.
QEMU tests cover persistence across cold boots through hubs, delayed storage,
RAM recovery, custom/fallback fonts, and power actions with/without the boot disk.
The shared `qemu_storage_smoke.py` harness also runs the ext4 persistence suite
when invoked directly. Older local FAT-specific fixtures are retired.

Protocol/port tests exercise descriptor bounds, SuperSpeed companions, route
depth, USB 2 compatibility, root resets, debounce, async completion ownership
and sibling device isolation. QEMU hubs exercise USB 2; physical SuperSpeed
and Surface hardware still require their own retests after driver changes.
