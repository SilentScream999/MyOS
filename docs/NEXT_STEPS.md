# Near-term platform work

Updated 2026-11-07. These are planned changes, not features already implemented.
Current behavior and verification are in [AI_HANDOFF.md](AI_HANDOFF.md) and
[PERSISTENT_STORAGE.md](PERSISTENT_STORAGE.md).

## 1. Verify the combined build on hardware

The combined change includes the RTC taskbar clock, USB BOT/SCSI + FAT16 storage,
file deletion/timestamps, image refresh/FRESH mode, foreground shell/menu launches,
echo control, waiting and Ctrl+C cleanup. Build, host filesystem/image checks and
QEMU checks have passed. Earlier desktop/Start-menu work was hardware-tested;
this does not establish that the new storage or cancellation paths work on the
physical test laptop.

Boot the UEFI `.img` on a spare USB stick. Check clock/date, keyboard and mouse
alongside storage, create/read/delete across power cycles, and Ctrl+C on both
terminal-launched and menu-launched apps. Keep saved files backed up. ISO flashing
alone does not create the writable partition. `FRESH=1` resets the host image's
data; reflashing a physical stick replaces that stick's existing saved data too.

## 2. Runtime timezones and a shared clock service

Today `CLOCK_UTC_OFFSET_MIN` is a build-time number of minutes added to the RTC
reading. Default 0 treats the RTC reading as local wall time; QEMU uses
`-rtc base=localtime`. `CLOCK_SHOW_SECONDS` changes tray formatting. FAT creation
and modification timestamps use the same RTC/offset, at two-second precision.
There is no named-timezone selection, automatic DST, runtime clock setting,
userspace time syscall or network time synchronization.

Next:

1. Separate hardware RTC interpretation (`local` or `UTC`) from the display
   timezone. Preserve compatibility with a machine that shares its RTC with
   Windows. Changing a display zone must not silently rewrite the hardware clock
   or apply an offset twice.
2. Persist settings, initially in an 8.3-compatible path such as
   `/disk/config/clock.cfg`. Start with runtime fixed-offset selection, then add
   named zones and their DST rules. A zone identifier is not just a fixed offset.
3. Introduce a clock service/time syscall with an explicit UTC basis and local
   conversion for the tray and filesystem. Keep monotonic uptime/scheduling
   separate from wall-clock corrections.
4. Retain FAT's local civil-time encoding at the filesystem boundary. Existing
   FAT timestamps have no timezone metadata: do not reinterpret or rewrite old
   file dates merely because the display zone changes. If timestamps must be
   unambiguous through DST changes, choose a richer storage format later.

Acceptance: changing a persisted setting updates the tray without recompiling;
reboot retains it; UTC/local RTC modes work; offset/date rollover cases are tested;
new file times follow the documented policy without changing creation time on edits.

## 3. Persistent applications and disk usage

Today only `/disk` is persistent. `/bin`, `/wallpaper.raw` and the initial root
tree are unpacked from `initrd.tar` into RAMFS. The shell and Start menu use
`/bin/*.elf`; adding files to `/disk` does not yet add menu apps.

Use a persistent `/disk/bin` directory mounted at `/bin`. Extend the current
single mount-point mechanism to support directory mounts before changing app
paths. Seed bundled ELF programs on first use, and keep a RAMFS recovery shell
when storage is absent or offline. Retain `/bootlog.txt` as a live RAM-backed view.

Define updates explicitly: refresh bundled system programs from the build while
preserving user-added executables and settings. Merely mounting an old persistent
`/bin` over the current initrd can boot an outdated shell after an OS upgrade.
Normal image refresh currently preserves the entire data partition unchanged;
updating files inside it requires additional filesystem-aware tooling. `FRESH=1`
remains the intentional reset operation, including future persistent apps/settings.

Add disk capacity/free-space reporting, directory creation in the shell, truncation,
rename and directory removal as separate filesystem milestones. For larger disk
loads, replace interrupt-disabled synchronous I/O with an asynchronous block queue
and safe sleeping locks. Then add internal-drive support and an installer; the
USB block interface is the transport abstraction to reuse.

Acceptance: a copied app survives reboot and appears in the menu; updating the OS
refreshes bundled programs without deleting added apps; missing disk still permits
recovery; image refresh and factory reset have distinct, tested outcomes.

## 4. Wallpaper: disk file to RAM once, after storage mounts

Recommended target: a disk-backed setting selects an asset such as
`/disk/desktop/wall.raw`. These names fit the current FAT16 8.3 restriction;
`wallpaper.raw` does not. Retain WRAW/BGRA support for now rather than requiring
new image decoders as part of this storage change.

Read and validate the asset once, convert/scale it into the compositor's RAM
wallpaper buffer, release the temporary file buffer, then redraw. Repaint from
that RAM buffer every frame. Do not read the USB disk inside the compose loop.
Reload only when the wallpaper setting/asset explicitly changes. Load into a
temporary buffer and swap only after complete successful validation; on a missing,
short, corrupt or unreadable file keep the existing/procedural background.

Boot the initial desktop with a procedural or small bundled fallback. After USB
enumeration and `/disk` mount, apply the user's wallpaper. Currently `wm_init`
and the compositor start before USB storage is discovered, so a disk wallpaper
cannot simply be opened at the existing early wallpaper-load call. Large reads
can still stall the current synchronous driver; moving the call to a worker alone
does not fix the driver's interrupt-disabled I/O. Treat responsive background
loading as dependent on the block-I/O work above.

Today wallpaper is bundled in the initrd and also passed as a standalone Limine
module, causing duplicate boot data. Keep this working path until disk loading and
fallback are implemented and tested. Then remove the duplicate standalone module;
Limine should load boot essentials, while desktop assets come through the VFS.
When retaining module fallbacks, identify modules by path/type, not array position:
the present configuration lists initrd, init.elf, then wallpaper, while the old
wallpaper fallback assumes module index 1. That index actually names init.elf.

Acceptance: wallpaper changes survive reboot; boot succeeds with the data disk or
asset missing; corrupt images cannot replace a valid background; the compositor
does no disk I/O per frame; duplicate boot wallpaper copies are removed only after
the replacement path is verified.

## 5. Continue app and desktop work

Add Paint as a normal ELF application, persist its documents under `/disk`, and
keep application discovery driven by files. Follow the existing GPU roadmap for
alpha, retained layers, hardware cursor and page flipping. Keep GPU bring-up
opt-in. Tighten window-ID ownership checks, user-pointer/ELF validation and user
mapping cleanup before treating the application API as a security boundary.
