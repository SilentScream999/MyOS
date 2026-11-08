# Persistent system storage

Updated 2026-11-07. The USB image contains an EFI boot partition and a writable
FAT16 system partition. The system partition mounts at `/`; applications do not
use a `/disk` prefix. This is a USB system root, not an internal-drive installer.

## Build and boot

From WSL in the repository:

```sh
make usb-image       # refresh boot files AND bundled disk programs; preserve user files
make run-usb         # build and boot the disk image directly in QEMU
make iso             # build recovery/live ISO and refresh the USB image
make run             # boot ISO in QEMU, with the USB system image attached
```

Use `wyzar-usb.img` for physical USB testing. Write the whole image with your usual
imaging program. Reflashing the whole image replaces the physical stick's saved
files, even though refreshing the host image preserves its saved files. An
in-place updater for a running/physical installation is not implemented.

`PERSIST_DISK=another.img` selects a different host image. `make clean` leaves disk
images alone. Close QEMU before updating its image; the updater locks the image
and atomically publishes a completed replacement. It never opens physical drives.
A failed update leaves the original image intact. The updater accepts only this
project's recognized image layout and validates the system FAT before editing it.

`FRESH=1` intentionally resets all saved files, settings, applications and wallpaper:

```sh
make usb-image FRESH=1
```

## Layout and root selection

| Partition | Size | Purpose |
|---|---|---|
| WYZARBOOT | 128 MiB | EFI boot, Limine, kernel, small recovery initrd and init module |
| WYZARDATA | 64 MiB | FAT16 persistent system root `/` |

The existing partition layout/labels remain compatible with older images.
Normal refresh migrates an older data volume without formatting it. A file formerly
seen as `/disk/hello.txt` is now `/hello.txt`; its bytes are preserved.

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
The compositor reads RAM on every frame; it never reads the disk. The recovery
initrd and Limine module list contain no duplicate wallpaper assets.

Reads use 64 KiB filesystem chunks to permit preemption between synchronous
operations. FAT16 follows FAT links to combine physically contiguous clusters
into requests of up to 128 sectors, handling unaligned heads/tails separately.
Fragmented links stop a run. The USB driver issues READ(10) commands for up to
128 sectors (64 KiB). Its DMA buffer is a physically contiguous, 64 KiB-aligned
subrange of a retained 128 KiB allocation, so a transfer cannot cross xHCI's
64 KiB buffer boundary. Other block backends can omit the optional
multi-sector callback and retain one-sector reads. Bounds are checked at the block,
partition and READ(10) boundaries. Writes remain one sector per command.

Sequential cluster hints and a FAT-sector cache avoid repeated metadata I/O;
FAT writes invalidate cached metadata. Large assets can still slow boot; this is
not an asynchronous I/O implementation. Wallpaper attempt duration is logged in
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

```text
ls /
ls /bin
write /home/hello.txt hello-from-wyzar
cat /home/hello.txt
stat /home/hello.txt
rm /home/hello.txt
open bootlog
```

The shell's `write` creates a file or overwrites from offset zero without truncating.
For a shorter replacement setting, remove the old file first, then write the new
one. For example: `rm /etc/desktop.cfg`, then
`write /etc/desktop.cfg /home/blue.raw`. Runtime wallpaper reload is not implemented.

Disk filenames are case-insensitive DOS 8.3 names. FAT16 currently supports
read/write/create/unlink/stat/mkdir/readdir. No seek/truncate/rename/rmdir syscall,
shell mkdir command, long-name lookup, sparse files or directory growth exists.
Root has 512 entries; newly created subdirectories have 126 usable slots.
`rm` refuses directories, read-only/open files, long-name aliases and corrupt chains.
Writes flush data, FAT metadata and directory changes; successful completion
requires the USB device to honor SCSI SYNCHRONIZE CACHE.

FAT times use RTC/local civil time at two-second precision, without timezone
metadata. Creation time survives edits. Filesystem/USB I/O still disables
interrupts during each synchronous operation; asynchronous requests, sleeping
locks and BOT recovery remain future work. FAT16 has no journal, so power loss can
leave partial writes, leaked clusters or inconsistent metadata.

Only USB BOT/SCSI LUN 0, 512-byte sectors, READ(10), MBR and FAT16 are supported.
No UAS/GPT/FAT32/exFAT/internal SATA/NVMe support exists.
USB 2.0-class hub enumeration retains registered storage devices and supports
nested route contexts; SuperSpeed hub descriptors/enumeration remain a separate
compatibility gap. Test physical hardware
before relying on the new root/asset path. The next filesystem should be chosen
before the installer/everyday-storage milestone; see [NEXT_STEPS.md](NEXT_STEPS.md).

## Verification

Development checks live in the locally ignored `tests/` directory:

```sh
clang++ -std=c++20 -fsanitize=address,undefined tests/scsi_retry_test.cpp -o .storage-test/scsi-retry-test
.storage-test/scsi-retry-test
python3 tests/system_volume_test.py
python3 tests/image_update_test.py
# Live-log callback/mounted metadata checks:
clang++ -std=c++20 -fsanitize=address,undefined -iquote src/core tests/live_log_test.cpp -o .storage-test/live-log-test
ASAN_OPTIONS=detect_leaks=0 .storage-test/live-log-test
clang++ -std=c++20 -fsanitize=address,undefined -iquote src/core -iquote src/drivers -iquote src/fs tests/storage_test.cpp -o .storage-test/storage-test
ASAN_OPTIONS=detect_leaks=0 .storage-test/storage-test .storage-test/root.img
clang++ -std=c++20 -fsanitize=address,undefined tests/wallpaper_test.cpp -o .storage-test/wallpaper-test
.storage-test/wallpaper-test
python3 tests/qemu_storage_smoke.py --image .storage-test/root.img --features
python3 tests/qemu_root_variants.py
python3 tests/qemu_hub_storage_test.py
```

Prepare the disposable image using `python3 tools/make_usb_image.py --output
.storage-test/root.img` after `make all`; QEMU variants also require `make iso`.
Host checks cover migration, upgrades, preservation, reclamation, full-volume and
corruption rejection, plus VFS/FAT operations and wallpaper bounds/decoding.
Read checks cover batching, fallback backends, overflow/bounds, unaligned and
fragmented files, failed reads and FAT cache behavior. Live log checks verify
growth and matching stat/read/directory sizes, including mounted placeholders.
The QEMU `qemu_boot_log_test.py` check additionally runs a userspace probe built
from `tests/log_metadata_probe.c` (place it at `.storage-test/log-probe.elf`)
and `tests/read_hash_probe.c` (place it at `.storage-test/read-hash-probe.elf`). It
compares the complete wallpaper hash after 64 KiB reads, and verifies the splash/desktop handoff and log stat/read/readdir agreement.
QEMU checks cover cold boots, persistence across image updates, disk-selected
wallpaper pixels, corrupt/missing assets and RAM recovery without a valid disk.
Hub checks boot the actual USB image through one/two emulated hubs, compare the
full wallpaper hash and retain writes across cold reboots. Hardware verification
is still required.
