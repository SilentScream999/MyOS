# ext4 system storage

Updated 2026-11-11. ext4 is the confirmed system-filesystem direction. USB is a
permanent supported installation target; internal-drive installation and a
portable USB system must share the same block/VFS interfaces.

## Build and boot

Run from WSL/Linux with clang, make and e2fsprogs installed:

```sh
make usb-image   # refresh ext4 boot/system files; preserve user data
make run-usb     # boot the ext4 USB image directly in QEMU
```

Automatic USB capacity and fixed-size testing:

```sh
make iso FRESH=1           # reset to a small seed; prepare larger USB on first boot
make iso FRESH=1 DEBUG=1   # reset to a fixed 256 MiB root; never expand on boot
```

Normal new/fresh builds contain a 256 MiB ext4 seed and 128 MiB EFI partition:
the complete image stays 385 MiB regardless of the destination stick's capacity.
After flashing, first boot reads a checksummed setup policy in reserved sector 1,
checks the seed UUID and exact partition layout, and prepares a larger ext4 root
in the unused tail. It copies regular files/directories, rereads every file to
verify size/CRC, cleanly closes and flushes the new filesystem, then publishes
its partition entry. The boot splash reports setup phases. Later boots mount
the prepared root directly; preparation does not repeat.

The original seed remains reserved as a recovery source. Usable root capacity is
approximately the stick's capacity minus 385 MiB, including the boot partition
and seed. A 1 GiB stick gets a 639 MiB root. If the tail would be no larger than
the seed, keep the original 256 MiB root. This is replacement provisioning,
not in-place resizing; reclaiming the seed and online growth remain future work.
Setup time depends on USB speed, metadata formatting and copied file sizes;
it does not zero the whole stick. Leave it connected until setup completes.

Failures before publication retain the original partition entry and select RAM
recovery; reboot retries setup. An uncertain partition write offlines storage
until reboot. Sector publication still depends on the device honoring flushes;
power loss or a torn MBR write can require reflashing. Unsupported file types or
an oversized inventory abort setup rather than silently omitting files. This
copies file contents/directories, not Linux permission/ownership/link semantics.

`DEBUG=1` controls image policy, independently of optimization/timing flags.
It fixes root at 256 MiB (385 MiB complete image), including on larger sticks.
Ordinary image updates preserve files and capacity and refresh the policy;
use `FRESH=1` to reset a previously enlarged host image to debug size.
Fresh resets now default back to the small seed. The legacy `ROOT_MIB` and
offline `tools/expand_usb_image.py` remain available for development fixtures;
manual host expansion is no longer required for the standard USB workflow.

Image creation/refresh/expansion stream data with bounded buffers. Native Linux
storage supports sparse output; Windows/DrvFS output is dense because sparse
reads were unreliable in integration tests. Allow disk space for the output and
private staging copies. RAM usage no longer scales with root capacity.

The standard USB image and default `PERSIST_DISK` is `wyzar-usb.img`, regardless
of filesystem. The current image has already been migrated to ext4. If there is
no image, `make usb-image` creates a new ext4 image. Ordinary refresh preserves
the existing ext4 system volume. `FRESH=1` explicitly resets the selected image.
The kernel and image updater support ext4 system roots only. FAT16 root support,
legacy image creation and migration tooling have been removed. The boot
partition remains FAT16 for firmware compatibility; the kernel does not mount it.
Older FAT-root images are rejected by ordinary updates; use a fresh image.

The whole `.img` is what gets written to a USB stick. Reflashing a physical stick
replaces its saved data; refreshing the host image does not update a stick in
place. No tools in this change open physical disks or change internal drives.

New image layout (MBR, 512-byte sectors):

| Partition | Size | Purpose |
|---|---|---|
| WYZARBOOT | 128 MiB | FAT EFI bootloader, kernel, recovery initrd |
| WYZARDATA | 256 MiB seed; automatic larger root at first boot | Journaled ext4 system root `/` |

256 MiB remains the seed/debug allocation. Tools accept aligned roots from
64 MiB up to the current MBR/READ(10) limit (total disk below 2 TiB), subject to
the supported ext4 feature profile. Larger sizes must fit the actual stick.
GPT, READ/WRITE(16), internal disks and online filesystem growth remain future
work. Automatic provisioning passed on a 1 GiB QEMU disk, formatter geometry
passed through 32 GiB with Linux checks; maximum-capacity hardware is unverified.

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
- Block writes use bounded multi-sector USB commands, up to 64 KiB. Backends
  without multi-sector writes retain the sector callback fallback.
- Underlying device flush after every completed library block write on mounted roots. This
  conservatively orders journal records, commits and checkpoints without relying
  on volatile USB write caches. Device errors latch the volume offline; there is
  no write retry or silent RAM-save fallback. File calls still block their caller.
  Redundant flush requests with no intervening writes are skipped; journal
  barriers are retained. Partial write/flush failure still fails the volume.
  First-boot unpublished targets defer flushes until format/copy completion;
  they become visible only after verification and the final durable close.
- Sequential directory listing resumes from a saved byte offset. Create/remove
  invalidates that directory's cursor; out-of-order indexed requests restart
  correctly. Cached vnodes use parent/name hash buckets rather than one global
  linear search. This avoids repeated scans for each sequential entry.
- File calls block their calling task, but normal runtime USB waits now sleep.
  A reentrant filesystem mutex serializes ext4/RAMFS operations without masking
  interrupts for the whole transaction. Exact slot/endpoint/TRB completion
  ownership lets the input task deliver disk completions and wake their owner
  while processing keyboard/mouse events. Initial discovery and hub/control
  probes retain their bounded synchronous path. Refcount cleanup uses a short
  IRQ guard so scheduler cleanup cannot wait recursively on the filesystem lock.
- Rename is available without overwriting a destination. Atomic replacement
  uses a durably written sibling temporary file, then one journal transaction
  to exchange existing regular-file references (or rename into an absent name).
  Cleanup follows publication. A crash may leave `.wy-save-N` containing the
  old contents, but the destination must remain complete old/new contents.
  Open destinations and mounted runtime placeholders are protected.
- Host updates edit an unpublished partition copy, check it with e2fsck, update
  bundled programs/assets, check it again, then atomically publish the whole
  image. Existing user files/apps/settings/wallpaper remain. Defaults are seeded
  only if absent.

## Verification

```sh
python3 tools/make_usb_image.py --output .storage-test/ext4-usb.img --fresh
python3 tests/run_ext4_tests.py
python3 tests/ext4_image_test.py
python3 tests/atomic_save_test.py
python3 tests/capacity_image_test.py
python3 tests/qemu_storage_api_test.py
python3 tests/qemu_storage_async_test.py
python3 tests/qemu_ext4_test.py
```

Host tests exercise the actual VFS adapter under address/undefined-behavior
sanitizers, UTF-8/long names, directory growth, cross-block I/O, deletion, failure
containment, unsupported features, cold recovery and Linux filesystem checking.
Image tests cover creation, update preservation, legacy-root rejection and failure atomicity. QEMU
tests boot the USB system three times, verify wallpaper loading, save a long-named
document, abruptly power off, and verify recovery/persistence and e2fsck results
on a copy. The third boot shuts down normally and must leave a clean filesystem.
Physical hardware still needs testing. These tests do not establish
correctness for every possible power-cut point or every ext4 feature.

Automatic setup regression checks:

```sh
python3 tests/run_autogrow_tests.py
python3 tests/qemu_autogrow_test.py
```

The native tests inject target-format/copy and partition-publication failures,
retry from the seed, reject invalid policy/UUID, and check fixed/small media and
32 GiB formatter geometry with e2fsck. QEMU boots normal and DEBUG=1 twice each.

## Remaining desktop-storage work

The VFS supports `O_TRUNC`, descriptor truncation, sync, rename and atomic saves
on ext4/RAMFS. Shell `write` uses atomic replacement; `append` extends content,
and `mv` renames without overwriting an existing destination. Growth through truncate is
explicitly zero-filled; descriptor offsets are preserved. These are synchronous
operations. Symlinks/hard links, permission enforcement, user/group
management and a UTC timestamp service remain separate work. The adapter leaves
civil timestamps unknown rather than pretending the current local RTC basis is
UTC. Applications using `O_TRUNC` directly still have non-atomic overwrite semantics.
Directory deletion/open-file behavior retains the prototype's restrictions.
Cached vnode metadata (including deleted nodes retained to reject stale lookup
pointers safely) is currently kept until reboot; cache reclamation remains work.

Next: safe removal, stable boot-volume
identity, runtime repair/recovery tooling, then GPT and NVMe/AHCI support. An
installer should prepare an internal system volume and bootloader while retaining
portable USB mode. No internal-drive installer is implemented in this change.

Dependency licensing and write-ordering notes:
[third_party/lwext4/WYZAR-NOTES.md](../third_party/lwext4/WYZAR-NOTES.md).
