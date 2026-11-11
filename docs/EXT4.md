# ext4 system storage

Updated 2026-11-10. ext4 is the confirmed system-filesystem direction. USB is a
permanent supported installation target; internal-drive installation and a
portable USB system must share the same block/VFS interfaces.

## Build and boot

Run from WSL/Linux with clang, make and e2fsprogs installed:

```sh
make usb-image   # refresh ext4 boot/system files; preserve user data
make run-usb     # boot the ext4 USB image directly in QEMU
```

The standard USB image and default `PERSIST_DISK` is `wyzar-usb.img`, regardless
of filesystem. The current image has already been migrated to ext4. If there is
no image, `make usb-image` creates a new ext4 image. Ordinary refresh retains
the filesystem already in an image. `--filesystem fat16` remains available for
creating legacy test images. `FRESH=1` explicitly resets the selected image.

For another legacy image, optional migration creates a new destination and never
modifies the source:

```sh
make migrate-usb LEGACY_IMAGE=old-fat16.img PERSIST_DISK=new-usb.img
```

The destination must not already exist. Without overrides, this target reads
`wyzar-usb-fat16.img` and creates `wyzar-usb.img`.

The whole `.img` is what gets written to a USB stick. Reflashing a physical stick
replaces its saved data; refreshing the host image does not update a stick in
place. No tools in this change open physical disks or change internal drives.

New image layout (MBR, 512-byte sectors):

| Partition | Size | Purpose |
|---|---|---|
| WYZARBOOT | 128 MiB | FAT EFI bootloader, kernel, recovery initrd |
| WYZARDATA | 256 MiB | Journaled ext4 system root `/` |

The ext4 image size is a starting allocation, not ext4's capacity limit. Runtime
partition/block callbacks are bounds-checked; GPT and larger image layouts remain
future work. The USB transport still uses READ/WRITE(10).

## Implemented behavior

- lwext4 source snapshot pinned to revision
  `58bcf89a121b72d4fb66334f1693d3b30e4cb9c5`; source/license notices retained.
- Read-only discovery of Linux-type MBR partitions labelled WYZARDATA. Ambiguous
  system roots select RAM recovery. Journal replay begins only after selection.
- A deliberately restricted ext4 profile: 4 KiB blocks, 256-byte inodes, internal
  journal, extents, filetype, directory indexing, sparse superblocks, large-file
  feature and extra inode space. Unknown feature bits are rejected before mount.
  This is not general write support for arbitrary Linux-created ext4 volumes.
- Existing VFS operations: create/read/write/seek, mkdir, empty rmdir, unlink,
  readdir, stat and free-space reporting. Names are case-sensitive and can hold
  UTF-8 bytes, spaces and up to 255 bytes per component. No Unicode normalization
  or case folding is performed. Current userspace paths remain limited to 255
  bytes total; the kernel adapter has a 511-byte path limit.
- Directories grow beyond the previous FAT16 slot limits. App discovery and
  wallpaper/font loading continue through VFS. `/tmp`, `/run` and logs retain
  their RAM mounts.
- Journal replay/start on mount; journal stop/unmount before shell shutdown or
  restart. If the disk is removed or fails, report an unsuccessful clean close
  and continue the power operation; the next mount must recover the journal.
- Underlying device flush after every completed library block write. This
  conservatively orders journal records, commits and checkpoints without relying
  on volatile USB write caches. Device errors latch the volume offline; there is
  no write retry or silent RAM-save fallback. I/O is still synchronous.
- Host updates edit an unpublished partition copy, check it with e2fsck, update
  bundled programs/assets, check it again, then atomically publish the whole
  image. Existing user files/apps/settings/wallpaper remain. Defaults are seeded
  only if absent.
- FAT16 migration validates the old filesystem, copies files/directories into a
  new ext4 volume, and installs the current OS. Short names become lowercase;
  known wallpaper/font configuration paths are adjusted for that case change.
  File bytes are preserved; historical timestamps/attributes are not migrated.
  Unsupported long-name FAT entries cause failure rather than silent omission.

## Verification

```sh
python3 tools/make_usb_image.py --output .storage-test/ext4-usb.img --fresh
python3 tests/run_ext4_tests.py
python3 tests/ext4_image_test.py
python3 tests/system_volume_test.py
python3 tests/qemu_ext4_test.py
```

Host tests exercise the actual VFS adapter under address/undefined-behavior
sanitizers, UTF-8/long names, directory growth, cross-block I/O, deletion, failure
containment, unsupported features, cold recovery and Linux filesystem checking.
Image tests cover migration, update preservation and failure atomicity. QEMU
tests boot the USB system three times, verify wallpaper loading, save a long-named
document, abruptly power off, and verify recovery/persistence and e2fsck results
on a copy. The third boot shuts down normally and must leave a clean filesystem.
Physical hardware still needs testing. These tests do not establish
correctness for every possible power-cut point or every ext4 feature.

## Remaining desktop-storage work

ext4 does not itself add missing OS APIs. Rename/truncate/fsync interfaces,
atomic replacement saves, symlinks/hard links, permission enforcement, user/group
management and a UTC timestamp service remain separate work. The adapter leaves
civil timestamps unknown rather than pretending the current local RTC basis is
UTC. The shell's existing `write` still preserves trailing bytes on overwrite.
Directory deletion/open-file behavior retains the prototype's restrictions.

Next: asynchronous block I/O and sleeping locks, safe removal, stable boot-volume
identity, runtime repair/recovery tooling, then GPT and NVMe/AHCI support. An
installer should prepare an internal system volume and bootloader while retaining
portable USB mode. No internal-drive installer is implemented in this change.

Dependency licensing and write-ordering notes:
[third_party/lwext4/WYZAR-NOTES.md](../third_party/lwext4/WYZAR-NOTES.md).
