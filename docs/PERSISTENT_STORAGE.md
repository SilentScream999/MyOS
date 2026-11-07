# Persistent USB storage

The USB stick can now contain both the bootable OS and a writable FAT16 data
partition. The initial system tree still comes from the initrd into RAMFS;
files under `/disk` go through the xHCI mass-storage driver to the stick.
This is the first persistent-storage milestone, not an internal-disk installer.
For upcoming runtime timezones, persistent `/bin`, disk usage and wallpaper
loading, see [NEXT_STEPS.md](NEXT_STEPS.md). `/bin` and wallpaper are still loaded
from the initrd into RAMFS today; the recommended wallpaper target is a file read
once from disk after mount, retained in RAM for composition, with a boot fallback.

## Build and boot

From WSL in the repository:

```sh
make usb-image
```

This creates `wyzar-usb.img` (193 MiB), using only the existing compiler toolchain
and Python 3. If it already exists, it rebuilds the boot partition from the current
kernel, shell and initrd, preserving the entire `/disk` data partition byte for byte.
`make clean` leaves USB images alone. Close QEMU before rebuilding: image updates
take an exclusive lock and refuse an image in use. A validated update is published
atomically so a failed build leaves the previous complete image.

For a completely fresh image with an empty data volume (apart from README.TXT),
choose a new filename:

```sh
make all
python3 tools/make_usb_image.py --output wyzar-new.img
```

For repeated testing, reset the usual image with a build flag:

```sh
make clean && make run FRESH=1
```

`FRESH=1` deliberately erases all saved `/disk` files in `PERSIST_DISK` and builds
a clean data volume containing README.TXT. It works with `make iso`,
`make usb-image`, and `make run-usb` too. With no flag, saved files are preserved.
For example, the longer workflow is
`make clean && make iso FRESH=1 && make run`. Close QEMU before either kind of
rebuild. The script's corresponding option is `--fresh`.

Use your image-writing program to write the **whole `.img` file** to a USB stick,
then boot its UEFI entry. Writing the image replaces the stick's partition table
and contents; back up existing files first. The ISO remains a separate live-boot
option and does not itself gain persistent storage when flashed unchanged.

The image has an MBR and two partitions:

| Partition | Size | Purpose |
|---|---|---|
| `WYZARBOOT` | 128 MiB | EFI system partition, Limine, kernel and boot modules |
| `WYZARDATA` | 64 MiB | Writable FAT16 volume mounted at `/disk` |

The kernel only probes MBR partition types `0x06`/`0x0e` with a valid FAT16 BPB
and the exact BPB volume label `WYZARDATA`. It never formats devices and does
not mount the EFI boot partition for writes. Currently the first matching volume
is selected; connect only one designated data volume. Selection by Limine's boot
disk identity is a future improvement. A CD/ISO boot can also use a separate
prepared USB data volume with this layout.

For normal development in QEMU, supply `OVMF.fd`, then:

```sh
make run
```

This boots the freshly built ISO and attaches `wyzar-usb.img` as a USB storage
device. Every `make iso` also invokes `make usb-image`, refreshing its boot files
while retaining saved data. Therefore `make clean && make iso && make run` produces
an up-to-date ISO and USB image, with saved files retained between builds.
`PERSIST_DISK=some-other.img` selects another image.
The ISO itself is still read-only; the attached USB image holds persistent data.

To test booting directly from the USB image instead:

```sh
make run-usb
```

`run-usb` refreshes the boot partition before starting QEMU too. Running `make all`
alone builds kernel/userspace but does not refresh images; use `make iso` or
`make usb-image` to package them. The update script accepts only this project's
recognized image size, partition layout and labels, and never opens physical disks.
Refreshing a host image preserves the host image's data; reflashing the whole
image onto a physical USB stick still replaces that stick's saved files.

## Try persistence

In the OS terminal:

```text
ls /disk
write /disk/hello.txt hello-from-wyzar
cat /disk/hello.txt
stat /disk/hello.txt
restart
cat /disk/hello.txt
rm /disk/hello.txt
```

Filenames are case-insensitive DOS 8.3 names (for example `HELLO.TXT`). The shell's
`write` starts at offset zero, creates missing files, and **does not truncate** an
existing file: shorter replacements leave trailing bytes. Each write flushes data,
FAT changes, and the directory entry before returning success. No separate sync
command is needed. Successful flushes depend on the device honoring SCSI
SYNCHRONIZE CACHE.

`rm <path>` deletes a closed, writable, short-name file on `/disk` and frees its
cluster chain. It refuses directories, read-only files, open files, long-name
aliases, and corrupt/cyclic chains. Deletion persists across reboots. `stat <path>`
shows size, creation time and modification time. Creation time stays fixed during
edits; modification time advances on successful data writes. Times are civil local
times from the same RTC/offset as the desktop clock. New creation and modification
times both use two-second resolution; `stat` displays both at that precision,
including older imported files whose creation time contains an odd second. A
write within the creation interval may correctly have the same displayed time.
This avoids the previous odd-second creation/even-second modification mismatch.
Missing/invalid RTC timestamps
display as `unknown`; older files are not assigned invented creation times.
RAMFS and the FAT16 root directory have no recorded timestamps yet.

## Implementation and current limits

* `src/block.h`: transport-neutral one-sector read/write/flush interface. Future
  AHCI/NVMe drivers can implement this same interface.
* `src/usb_storage.h`: xHCI bulk endpoints, USB Bulk-Only Transport CBW/data/CSW,
  SCSI readiness/sense, capacity, READ(10), WRITE(10), and SYNCHRONIZE CACHE.
  Transfer completions must match their TRB pointer, endpoint and slot. Unrelated
  HID and port events are copied into a bounded queue for normal dispatch.
* `src/fat16.h`: BPB and partition bounds validation, mirrored FAT updates, short
  names, cached vnodes, file I/O, creation, deletion, directory creation, timestamps
  and enumeration. Deleted vnode objects remain invalidated if their slot is reused.
* `src/vfs.h`: `/disk` mount traversal. Missing files require `O_CREAT` (`0x40`);
  `O_RDONLY=0`, `O_WRONLY=1`, and `O_RDWR=2` are enforced. Initrd extraction uses
  `O_WRONLY | O_CREAT` explicitly. Oversized path components are rejected.
* `tools/make_usb_image.py`: standard-library-only image builder. Boot files use
  VFAT long entries where Limine filenames require them; writable files use 8.3.

Only transparent SCSI over USB BOT, LUN 0, 512-byte logical sectors and READ(10)
addressing are supported. UAS, GPT, FAT12/32, exFAT and internal SATA/NVMe drives
are not supported. Stalls, transport errors and timeouts offline the disk until
reboot; BOT reset recovery is deferred. Devices that reject cache flushes cannot
complete successful writes. Hardware compatibility remains to be tested.

FAT16 supports read/write/create/unlink/stat/mkdir/readdir through the VFS.
Syscalls 4 (stat) and 87 (unlink) expose metadata and deletion; the metadata layout
is shared in `src/fileinfo.h`. No rename, directory deletion, truncate, seek syscall,
long-name lookup, sparse files or directory growth yet. The root directory has 512 entries; subdirectories
created by this implementation have one cluster (126 usable slots with this
image). The shell currently has no mkdir command; the VFS operation is present.
`run /disk/APP.ELF` can use the existing loader for files placed there, but Start
Menu discovery still uses `/bin` in RAMFS.

I/O and controller resets are serialized by disabling interrupts on this
single-core kernel. I/O uses bounded busy waits; large operations can pause the
desktop. An asynchronous request queue and sleeping locks are needed before
larger workloads or SMP. A controller reset, unplug or transport failure leaves
existing mounted vnodes offline; reconnecting requires a reboot to regain `/disk`.
Up to eight storage device instances are retained per boot so old handles cannot
accidentally address a replacement device.

FAT16 is not journaled. Flush ordering limits exposure, but power loss during a
write can still leave partial overwrites, leaked clusters or inconsistent metadata.
This first backend is intended for development data with backups.

## Verification

Host tests use the actual VFS and FAT16 implementation with a memory block device:

```sh
mkdir -p .storage-test
python3 tools/make_usb_image.py --output .storage-test/usb.img
clang++ -std=c++20 -fsanitize=address,undefined tests/storage_test.cpp -o .storage-test/storage-test
ASAN_OPTIONS=detect_leaks=0 .storage-test/storage-test .storage-test/usb.img
```

Tests cover multi-cluster I/O, unaligned overwrites, fresh mounts, directories,
name/partition/BPB validation, full disks, failed writes, failed cache flushes,
timestamp persistence, open/read-only deletion protection, cluster reclamation,
cyclic-chain rejection and directory-slot reuse.
VNode caches live for the mounted volume's lifetime; no unmount/free operation
exists yet, so leak detection is disabled for this host harness.

`python3 tests/qemu_storage_smoke.py` uses a disposable `.storage-test/usb.img`
containing the current build and `/usr/share/OVMF/OVMF_CODE.fd`. It boots through
USB, issues a shell write through the USB keyboard, terminates QEMU, verifies the
file directly in the image, cold-boots the same image, and issues a shell read.
Framebuffer captures and kernel logs are kept in `.storage-test/`.
Pass `--iso --image .storage-test/iso-usb.img` to test the `make run` configuration
using a separately generated disposable image while booting the current ISO.
Add `--features` for three boots that also test creation/modification timestamps,
file deletion, slot reuse, and a boot-image refresh between boots. Run
`python3 tests/image_update_test.py` for atomic image-update/data-preservation,
layout validation, exclusive creation, lock and simulated publication-failure tests.

Verified on 2026-11-07: kernel/userspace build (13 existing warnings), host tests
under ASan/UBSan, atomic image-update tests, and the three-cold-boot QEMU feature
test (timestamps, deletion, slot reuse, persistent files across a boot refresh).
`fsck.fat -n` reported no errors in the resulting data partition. Initial USB tests also exposed and
verified fixes for USB's runtime-initialized MMIO base and preemption during the
ELF loader's temporary CR3 switch. Physical-stick behavior is still unverified.

## Next milestones toward installation

1. Verify the boot stick on physical xHCI hardware, including storage and HID
   coexistence, flush behavior, disk-full behavior and unplug handling.
2. Add asynchronous block requests, BOT recovery and filesystem error reporting.
3. Add an internal-drive transport (NVMe or AHCI, based on target hardware).
4. Add general partition discovery, mount selection and a richer filesystem API.
5. Build an installer that shows the destination disk, prepares EFI/system/data
   partitions, copies Limine and OS files, and preserves a recovery USB path.
6. Move persistent applications/configuration into a disk-backed system layout,
   and add boot-partition upgrades that preserve user data.

Protocol reference: [USB-IF Bulk-Only Transport 1.0](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf).
Timestamp reference: [Microsoft file times and FAT precision](https://learn.microsoft.com/en-us/windows/win32/sysinfo/file-times).
Controller reference: [Intel xHCI specification](https://www.intel.com.br/content/dam/www/public/us/en/documents/technical-specifications/extensible-host-controler-interface-usb-xhci.pdf).
