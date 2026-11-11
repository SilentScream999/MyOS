# Near-term platform work

Updated 2026-11-10. Implemented foundations and planned follow-up work are marked below.
Current behavior and verification are in [AI_HANDOFF.md](AI_HANDOFF.md) and
[PERSISTENT_STORAGE.md](PERSISTENT_STORAGE.md).

## 1. Verify the combined build on hardware

### Storage stages authorized on 2026-11-10

The user requested all current storage catches/bottlenecks be addressed in stages.
Track the whole scope here; finishing a stage does not finish the remaining work.

1. **Implemented:** batched USB/ext4 writes (up to 64 KiB), removal of redundant
   flushes without new writes, sequential directory cursors with mutation
   invalidation, and hashed vnode lookup. Durability barriers are retained.
2. **Implemented:** `O_TRUNC`, descriptor truncate/sync, rename without overwrite,
   and atomic replacement on ext4/RAMFS. Shell `write` uses atomic saves, `append`
   extends content, and `mv` renames. Permission enforcement, user/group identity,
   links and consistent UTC timestamps need explicit OS semantics too.
3. **Implemented:** configurable new roots (`ROOT_MIB`), streamed image updates,
   and atomic offline image expansion via `tools/expand_usb_image.py`, including
   a target stick's exact byte capacity. Reflash the expanded host image; there
   is no automatic live/first-boot resizing. Current MBR/READ(10) bounds remain.
4. **Implemented:** a reentrant sleeping filesystem mutex and exact storage
   completion ownership; runtime USB waits sleep and wake on delivery while
   input/timers continue. Cold discovery and control probes retain their bounded
   synchronous path. Caller-facing file calls still block until completed.
5. **Pending:** safe eject and stable volume identity; controlled recovery from
   media removal, broader ext4 feature profiles, and internal-drive installation.
   Unplugging writable media during a save cannot guarantee that save survives.

The combined change includes the RTC taskbar clock, USB BOT/SCSI + ext4 storage,
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
`-rtc base=localtime`. `CLOCK_SHOW_SECONDS` changes tray formatting. The ext4
adapter leaves civil timestamps unknown until the clock has a defined UTC basis.
There is no named-timezone selection, automatic DST, runtime clock setting,
userspace time syscall or network time synchronization.

Next:

1. Separate hardware RTC interpretation (`local` or `UTC`) from the display
   timezone. Preserve compatibility with a machine that shares its RTC with
   Windows. Changing a display zone must not silently rewrite the hardware clock
   or apply an offset twice.
2. Persist settings, initially in an 8.3-compatible path such as
   `/etc/clock.cfg`. Start with runtime fixed-offset selection, then add
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

## 3. Disk-backed root and remaining storage work

Implemented: the WYZARDATA volume becomes `/` before userspace starts. `/bin`,
`/etc`, `/home`, and `/share` persist; `/tmp` and `/run` are RAM-backed mounts.
The initrd contains recovery applications, not the normal system tree or wallpaper.
`/bootlog.txt` and `/run/boot.log` expose the live RAM log.

Normal image refresh replaces bundled `/bin/init.elf`, `/bin/counter.elf`, and
`/bin/win_test.elf`, preserving added apps, documents, settings and existing
wallpaper. The host updater migrates old data volumes without reformatting them;
old `/disk/hello.txt` becomes `/hello.txt`. `FRESH=1` still resets all saved files.
Missing/invalid/ambiguous volumes select RAM recovery. Root selection happens once;
unplugging storage never silently redirects saves into RAM.

Implemented: capacity/free-space reporting (`df` / `SYS_STATFS`), shell `mkdir`
(`SYS_MKDIR`), directory removal (`rmdir` / `SYS_RMDIR` requiring empty directories),
file seeking (`SYS_LSEEK`), per-task CWD tracking (`SYS_GETCWD` / `SYS_CHDIR`),
relative path resolution (`.`, `..`, and subdirectories across all filesystem syscalls),
and shell `cd` / `pwd` with a dynamic `wyzar:<cwd>>` prompt.

Next: hardware-test atomic saves and runtime input responsiveness. Continue with
safe removal, stable volume identity and internal-drive support. Replace remaining
synchronous control probes with asynchronous control transfers. Generalize root selection to
stable boot-volume identity; currently exactly one WYZARDATA-labelled disk is
required, with `/etc/root.cfg` and `/bin/init.elf` checked before selection.

ext4 is confirmed and the initial USB system-root implementation is in place.
See [EXT4.md](EXT4.md) for the supported profile, migration, tests and limits.
USB remains a permanent supported system-drive target. Keep the FAT EFI boot
partition independent of ext4. Next complete everyday file APIs, responsive I/O,
stable boot-volume identity and recovery tooling, then add internal-drive support
and an installer using the same block/VFS abstractions.

## 4. Disk wallpaper

Implemented: `/etc/desktop.cfg` contains one absolute asset path, defaulting to
`/share/wall.raw`. Both settings and the asset persist across builds. Normal image
updates install the bundled wallpaper only when the default asset is absent.

Boot keeps the animated splash visible. After root selection, the asset is read
once, validated and scaled into a private RAM buffer, then swapped into the
compositor. Missing, short, corrupt or unreadable assets preserve the fallback.
The first desktop appears after loading succeeds or a fallback is selected.
The compositor does no disk I/O. WRAW/BGRA remains the supported format; the initrd
and standalone Limine wallpaper module copies have been removed.

Reads are bounded (15-second budget checked between chunks) and chunked so
preemption can occur between reads. ext4 uses multi-sector I/O and USB
READ/WRITE(10) batches up to 128 sectors with aligned DMA. USB ports share one 100 ms
settling window per controller instead of waiting separately on each empty port.
Boot phase timings identify discovery, readiness, read, decode and composition costs.
The USB backend remains synchronous. Runtime wallpaper selection/reload UI and fully
responsive asynchronous loading remain future work. Settings changes apply on
reboot. See [PERSISTENT_STORAGE.md](PERSISTENT_STORAGE.md) for file locations and
current editing limitations.

## 5. Continue app and desktop work

Add Paint as a normal `.wiz` or `.elf` application, persist its documents under `/home`, and
keep application discovery driven by files. Follow the existing GPU roadmap for
alpha, retained layers, hardware cursor and page flipping. Keep GPU bring-up
opt-in. Tighten window-ID ownership checks, user-pointer/ELF validation and user
mapping cleanup before treating the application API as a security boundary.

### Future `.wiz` app metadata

Implemented: `.wiz` and `.elf` currently contain the same static x86-64 ELF
binary and use the same WyzarOS ABI, loader and process lifecycle. Only the
filename extension differs. Both are discovered in `/bin` and launch through
the shell or Start menu; renaming between them requires no conversion.

Possible additions later:

- Embedded app icons for the Start menu and desktop.
- A display name independent of the filename, plus an app description.
- Version and publisher metadata for app information and updates.
- An optional versioned `.wiz` container/header holding metadata and the
  executable payload, with a packaging tool to create it from an ELF build.

Keep existing ELF-based `.wiz` files and ordinary `.elf` programs working if a
new container is introduced. These metadata/container features are planned
possibilities, not part of current `.wiz` support. See [APP_LOADING.md](APP_LOADING.md).
