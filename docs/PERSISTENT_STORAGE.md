# Persistent storage and recovery

Updated 2026-11-11. The normal root filesystem is journaled ext4 on USB.
FAT16 is used only for the firmware boot partition. See [EXT4](EXT4.md) for
the supported filesystem profile and write-ordering details.

External USB FAT32 and exFAT volumes mount under their volume labels, for example
`/mnt/TRAVEL`. Unlabeled volumes use a capacity-based name such as
`/mnt/USB Drive (1.9 GiB)`. Duplicate names receive a numbered suffix. Empty
mount folders from earlier boots are reused. Folders containing data are preserved.
Explorer's Devices view and drive sidebar update when volumes arrive or leave.
This supports primary MBR partitions of type 0x0B/0x0C (FAT32), type 0x07 (exFAT), and unpartitioned
media with 512-byte sectors. The system-root disk is excluded from this scan.
Files and directories support long UTF-8 names, reads, writes, rename and deletion.
Explorer copies files in either direction and can move individual files across
filesystems. FAT32's maximum file size is 4 GiB minus one byte. exFAT transfers
support files larger than 4 GiB. Creation and modification timestamps use the
local RTC, with January 1, 1980 as the fallback if the clock is unavailable.

External GPT/extended partitions, formatting and repair remain pending.
Eject in Explorer flushes filesystem metadata and the device, refuses open files,
then detaches every mounted volume on that physical drive. Devices without an
explicit cache-flush command display that limitation after logical eject.
Writes close and synchronize their FatFs handle before returning, and failed
I/O offlines that volume. The background mount worker removes unavailable mounts and reuses free
mount folder names when newly enumerated devices arrive. Open handles retain
the retired volume and cannot access replacement media.

Notepad and shell `write` use VFS replacement on external volumes. New contents
are staged in a `.wyzar-save-<number>` transaction folder with a validated target
record. The original is retained as a backup until publication. Interrupted saves
are recovered during lookup or folder browsing. FAT32/exFAT use this recovery
scheme without journaling. Power loss can still require filesystem repair,
especially on devices without explicit cache-flush support.

On the first browse of each folder after mounting, abandoned files matching
`.explorer-copy-<digits>.tmp` are removed. Open files and stages created during
the current mount are protected. Cleanup scans up to 8192 entries per browse.

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

Newly formatted journals are cleared in full before an expanded root is
published. Reusing journal blocks while restarting transaction IDs could replay
metadata from a previous filesystem after an abrupt removal, even after an idle
boot. The formatter regression reproduces this sequence on a file-backed disk
and verifies repeated recovery with independent filesystem checks.
This initialization fix prevents stale-record replay on newly formatted roots.
It does not reconstruct files already damaged by an earlier replay.
Clean journal shutdown also preserves the last committed transaction ID.
Resetting that ID to zero could make records from an older mount session
match the IDs of a later session.
Checkpoint callbacks retain their dirty buffers and committed journal records
when a device write fails. Journal shutdown reports the pending error instead
of advancing past failed writes or marking that session clean. The native
failure-injection suite checks allocation metadata after recovery at each
interrupted file-exchange boundary.

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
