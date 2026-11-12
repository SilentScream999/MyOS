# Persistent storage and recovery

Updated 2026-11-11. The normal root filesystem is journaled ext4 on USB.
FAT16 is used only for the firmware boot partition. See [EXT4](EXT4.md) for
the supported filesystem profile and write-ordering details.

## Build, update and flash

```sh
make usb-image       # create/refresh wyzar-usb.img, preserving saved files
make run-usb         # refresh and boot the USB image in QEMU
make iso             # also refresh USB boot/system files; build recovery/live ISO
```

`PERSIST_DISK=another.img` selects another host image. `make clean` leaves it
alone. `FRESH=1` deliberately resets saved files, apps, settings and wallpaper;
`DEBUG=1` fixes the root at 256 MiB. Repeat flags for each make invocation.
[BUILD_CONFIGS](../BUILD_CONFIGS.txt) has complete command examples.

Close QEMU before refreshing an image. The updater works on private copies,
checks the filesystem and publishes a completed replacement atomically. Failed
updates leave the original image intact. Windows/DrvFS output is dense; allow
space for the output and staging copies. Tools never open physical devices.

Flash the entire `.img`, not its ext4 partition or ISO, for persistent USB boot.
Reflashing replaces the stick's saved data. The host updater is not an in-place
updater for a running/physical installation; no internal-drive installer exists.

## Layout and capacity

| Area | Purpose |
|---|---|
| MBR/reserved sectors | Partition table and checksummed first-boot policy |
| `WYZARBOOT`, 128 MiB | FAT firmware boot, Limine, kernel and recovery files |
| `WYZARDATA`, initial 256 MiB | Journaled ext4 root `/` |

Normal fresh images occupy 385 MiB. On a larger stick, first boot can create a
larger root in unused tail space, copy/verify the seed and publish the replacement
partition. The original seed remains reserved, leaving roughly stick capacity
minus 385 MiB for the larger root. Small media retains the seed when expansion
would not help. `DEBUG=1` suppresses setup. [EXT4](EXT4.md) covers failure/retry
semantics and developer-only capacity tools.

## Boot selection and RAM recovery

Boot initially unpacks recovery files into RAMFS. After USB enumeration it accepts
exactly one valid `WYZARDATA` root, checks `/etc/root.cfg` for `WYZARROOT1` plus
newline, checks `/bin/init.elf`, and attaches runtime mounts before starting the
disk shell. Missing/invalid/ambiguous roots or a failed disk shell use RAM recovery.

| Path | Normal disk boot |
|---|---|
| `/bin`, `/apps`, `/etc`, `/home`, `/share` | Persistent system/app/settings/files |
| `/tmp`, `/run` | RAM mounts, cleared at boot |
| `/bootlog.txt`, `/run/boot.log` | Live RAM kernel log |
| `/dev`, `/mnt` | Reserved for future devices/mounts |

Once the disk shell reaches its first stdin request, the unused recovery tree,
archive and fallback-shell module are reclaimed. Runtime RAM/log mounts remain.
Failed disk boot retains the complete recovery environment and its procedural
wallpaper. Recovery changes are nonpersistent.

Root selection does not change after userspace starts. Disk removal/controller
failure offlines storage until reboot; saves are never redirected to RAM and
old handles are never rebound to a reconnected device. Shutdown/restart attempts
to close the journal, reports failure if the disk is gone, and continues the
requested power action.

## Update ownership

The updater replaces bundled `/bin/init.elf`, `/bin/counter.elf`,
`/bin/win_test.elf`, `/etc/root.cfg`, bundled Selawik font/license and dependency
license files. Use another executable name for personal replacements of bundled
programs. Missing defaults are seeded, including icon files/settings and the
default wallpaper. Existing user apps, manifests, configuration, wallpaper and
replaced default PNGs are preserved. Conflicting paths or invalid filesystems
fail the update rather than silently reformatting them.

The default wallpaper setting is `/share/wall.raw` in `/etc/desktop.cfg`; it can
instead name a PNG/JPEG file. `/etc/font.cfg` selects a TTF, and `/etc/icons.cfg`
selects an icon theme. Wallpaper/font settings apply on reboot; opening Start
refreshes app/icon resources. See [IMAGES](IMAGES.md) and [FONTS](FONTS.md).

## File operations

```text
mkdir /home/documents
write /home/documents/note.txt hello
append /home/documents/note.txt -again
mv /home/documents/note.txt /home/documents/saved.txt
cat /home/documents/saved.txt
df
```

`write` uses atomic replacement; `append` extends a file; `mv` does not overwrite
an existing destination. Open/mounted destinations are protected. Descriptor
truncate and sync APIs exist; direct `O_TRUNC` writes are not atomic saves.
Names are case-sensitive, up to 255 bytes per component; there is no FAT 8.3
restriction on the ext4 root. Current userspace paths are limited to 255 bytes.
Permissions, links and UTC file timestamps remain unfinished.

## Diagnosing startup/storage failures

Use `open bootlog` or `cat /bootlog.txt`. In recovery, `cat /run/storage.txt`
reports root-selection failure and USB topology; this RAM-only file is absent
on a successful disk boot. It distinguishes discovery, readiness, partition,
mount/journal, marker/shell and runtime-mount failures.

If no disk is online, boot allows a 1.5-second discovery grace period before
root selection. Per-device bounded probes can extend elapsed time. Later hotplug
does not replace a running recovery root. Readiness retries are limited to
recognized transient SCSI conditions; writes/flushes are not blindly replayed.
See [USB_INPUT](USB_INPUT.md) for protocol/port behavior.

Normal logs suppress extra timing counters. `BOOT_DIAGNOSTICS=1` enables them
for a clean diagnostic build. A QEMU window saying Paused can represent a guest
shutdown/triple fault because `make run` uses `-no-shutdown -no-reboot`; the
default-framebuffer regression is covered in [TESTING](TESTING.md).
