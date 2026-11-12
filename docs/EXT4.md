# ext4 implementation

Updated 2026-11-11. This is the filesystem/capacity implementation reference.
Build, flashing, runtime paths and diagnostics are in
[PERSISTENT_STORAGE](PERSISTENT_STORAGE.md); commands are in [TESTING](TESTING.md).

## Supported profile

The kernel/updater support ext4 system roots only. The FAT EFI partition remains
independent and is not mounted by the kernel. Legacy FAT-root images are rejected
by ordinary updates; conversion/migration tooling has been removed.

lwext4 is pinned to `58bcf89a121b72d4fb66334f1693d3b30e4cb9c5`, with local
atomic-exchange/error-propagation changes recorded in
[WYZAR-NOTES](../third_party/lwext4/WYZAR-NOTES.md). Licensing notices are retained.

The accepted profile uses 4 KiB blocks, 256-byte inodes, internal journal,
extents, filetype, directory indexing, sparse superblocks, large-file and extra
inode-space features. Unknown feature bits are rejected before mount; this is
not general write support for arbitrary Linux ext4 volumes.

Names are case-sensitive and accept up to 255 bytes per component, including
UTF-8 bytes/spaces, without normalization or case folding. Userspace paths are
limited to 255 bytes overall; the adapter has a 511-byte path limit. Unicode
filename storage does not imply Unicode text rendering/shaping.

## Capacity setup

Normal new/fresh images have a 256 MiB seed root and occupy 385 MiB total.
Reserved LBA1 holds a CRC-protected policy bound to the seed UUID/start/count.
On suitable larger media, first boot:

1. Validates policy, seed identity/layout and target bounds.
2. Formats an unpublished root in unused tail space.
3. Copies regular files/directories and rereads size/CRC for verification.
4. Cleanly closes/flushes the target, then publishes/readbacks the MBR root entry.

The seed stays reserved. A 1 GiB disk can receive a 639 MiB root; if the tail
would not be larger than the seed, the seed remains active. This is replacement
provisioning, not in-place resize. It preserves contents/directories, not Linux
permission, ownership or link semantics. Unsupported files/inventory overflow
abort rather than silently dropping entries.

Pre-publication failures preserve the original entry, select RAM recovery and
retry after reboot. Uncertain publication offlines storage. Device flush honesty
and sector atomicity still matter; torn MBR writes can require reflashing.
Unpublished targets defer barriers during format/copy; mounted roots retain them.

`DEBUG=1` keeps a fixed 256 MiB root. Ordinary host updates preserve capacity;
fresh resets return to the seed unless explicitly overridden. `ROOT_MIB` and
`tools/expand_usb_image.py` remain development tools for private image files,
not a required physical-stick workflow. Images use MBR and READ/WRITE(10), below
the current 2 TiB disk limit; GPT, wider commands and online growth are pending.
Native geometry checks reached 32 GiB, not maximum-capacity hardware validation.

## Files, transactions and errors

VFS supports create/read/write/seek, mkdir, empty rmdir, unlink, readdir, stat,
capacity reporting, truncate, sync, rename without overwrite and atomic save.
Truncate growth is zero-filled and preserves descriptor offsets.

Atomic replacement writes and durably closes a sibling temporary, then uses one
journal transaction to exchange regular-file references or rename into an absent
destination. Cleanup follows publication. A crash can leave `.wy-save-N` holding
old contents, but the destination must be complete old/new contents. Open
destinations and mounted runtime placeholders are protected. Direct `O_TRUNC`
overwrites do not provide this atomic-save contract.

The selected root replays/starts its journal at mount and closes/unmounts before
normal power actions. Device errors latch the volume offline, without write
retry, handle rebinding or RAM fallback. Failed clean close does not prevent a
requested shutdown/restart; the next mount must recover.

On mounted roots each completed library block write retains a device flush
barrier. Redundant flushes without intervening writes are skipped. Partial write
or flush failure still fails the volume. USB writes batch up to 64 KiB, with
sector fallback for backends without multi-sector callbacks.

## Runtime and host implementation

File calls block their caller, but runtime USB completion waits sleep. A
reentrant filesystem mutex serializes ext4/RAMFS work without masking IRQs for
whole transactions. Exact controller/slot/endpoint/TRB ownership lets the input
dispatcher deliver storage completions while handling HID. Refcounts use short
IRQ guards so scheduler cleanup does not wait recursively on the filesystem lock.
Discovery/control probes remain bounded synchronous operations.

Sequential directory calls resume by byte offset; mutations invalidate cursors
and out-of-order requests restart. Vnodes use parent/name hash buckets. Cached
vnode metadata, including deleted entries retained against stale lookup pointers,
currently remains until reboot; reclaiming that cache is pending.

Host updates stream private partition copies, check with `e2fsck`, install
bundled files/defaults, check again and atomically publish the image. Native Linux
output can be sparse; Windows/DrvFS output is dense because sparse reads were
unreliable in integration tests. Buffer usage is bounded, but staging still
requires disk space. Normal refresh preserves personal files and settings.

Remaining semantics: permissions/user identity, links, UTC timestamps, safe
eject/reconnect, stable boot-volume identity and broader ext4 features. The
adapter leaves civil timestamps unknown while RTC interpretation is undefined.
Testing scope and remaining installation work are in [NEXT_STEPS](NEXT_STEPS.md).
