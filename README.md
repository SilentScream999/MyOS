# WyzarOS

A hobby 64-bit x86 operating system: C++ kernel, C userspace, booted by [Limine](https://github.com/limine-bootloader/limine).
It has a framebuffer desktop (window manager, CPU compositor, taskbar, mouse cursor), USB (xHCI) and PS/2 input,
preemptive multitasking, an ELF loader and a ring-3 shell.

*Last updated: 2026-11-07.* Start with **[docs/AI_HANDOFF.md](docs/AI_HANDOFF.md)** for current status and **[docs/NEXT_STEPS.md](docs/NEXT_STEPS.md)** for runtime timezones, disk-backed apps/settings and wallpaper loading.

The long-term goal is a **Windows-like, GPU-composited desktop**. Where we are on that road, and what comes next, is in
[docs/GPU_DESKTOP_ROADMAP.md](docs/GPU_DESKTOP_ROADMAP.md). How programs should be started (instead of being hard-coded) is in
[docs/APP_LOADING.md](docs/APP_LOADING.md).

---

## Quick start

**Persistent USB storage:** build with `make usb-image` and boot the resulting
UEFI USB image. `/disk` is a writable FAT16 data volume; the ISO remains a live
RAMFS boot. See [docs/PERSISTENT_STORAGE.md](docs/PERSISTENT_STORAGE.md) for image
writing, persistence checks, limitations, and the path toward an installer.
`make iso` also refreshes the USB image's boot files while preserving `/disk` data.
In the shell, use `rm /disk/FILE.TXT` to delete a closed file and
`stat /disk/FILE.TXT` to display creation and modification times.
For a clean image on each test, use `make clean && make run FRESH=1`.
The flag resets saved `/disk` files; ordinary builds preserve them.
`run /bin/counter.elf` runs in the foreground: the shell waits for exit and then
prints a fresh prompt automatically. Keep `tools/make_usb_image.py` for image
builds; the `tests/` directory contains optional development checks.
Foreground programs start with keyboard echo off; interactive programs can enable
it with syscall 18 (`TTYECHO`). Unread input is discarded before the prompt returns.
Press Ctrl+C to terminate the foreground program, including `win_test` when its
window has focus. Start-menu programs use the same foreground launch path and
return to the terminal prompt on exit; only one foreground program runs at a time.

You need WSL on Windows (Ubuntu 22.04 works well) with `make`, `clang`, `clang++`, `ld.lld`, `xorriso` and `qemu-system-x86_64`.
The Makefile calls `clang`, `clang++` and `ld.lld` directly.

**Limine:** clone the *binary* branch into the `limine/` folder:

    git clone https://github.com/limine-bootloader/limine.git --branch=v11.1.0-binary --depth=1 limine

**Build and run**

    make clean && make iso          # builds wyzar, init.elf and wyzar.iso
    make run                        # QEMU (needs OVMF.fd in the project root; it is not in the repo)

QEMU has **no Intel GPU**, so anything GPU-related (the `GPU:` boot-log lines and the HUD GPU status) can only be tested by booting
`wyzar.iso` on real hardware. Our test machine is an Intel Gen9 laptop (Skylake/Kaby Lake class, HD 620).

**Build options** (pass them on the build command, and run `make clean` when you change them):

| Option | Default | Meaning |
|---|---|---|
| `OPT=-O2` | `-O0` | Kernel optimisation level. `-O2` compiles and links but has **not** been run on hardware; test it on its own. |
| `GPU=1` | `0` | Run the Intel GPU bring-up, self-test and benchmark at boot (briefly paints test squares). Off by default because the CPU copy is currently faster. |
| `CLOCK_UTC_OFFSET_MIN=-420` | `0` | Minutes to add to the RTC value for the taskbar clock. `0` suits Windows machines (they keep local time in the RTC); use e.g. `-420` if the RTC holds UTC and you are at UTC-7. |
| `CLOCK_SHOW_SECONDS=1` | `0` | Show seconds in the taskbar clock. |

The Makefile tracks header dependencies (`.d` files), so editing a header rebuilds the kernel. It does **not** notice changes to build
options, hence the `make clean`.

---

## What works today

* **Boot:** Limine, 64-bit long mode, framebuffer, SSE enabled, on-screen kernel log (`print()` feeds a 64 KB log buffer, readable as `/bootlog.txt` and in the Boot Log window).
* **CPU/interrupts:** GDT + TSS, IDT with naked stubs, 8259 PIC remap, PIT tick, TSC-based frame pacing.
* **Memory:** Limine memory map, physical frame allocator, 4-level paging, higher-half direct map, growable kernel heap (`kmalloc`/`kfree`), per-process address spaces.
* **Tasks:** PCB, context switch, round-robin **single-core** preemptive scheduler. Other cores are detected but never started.
* **Syscalls:** `syscall`/`sysret` entry and a small dispatcher (table below).
* **Userspace:** static ELF64 loading from VFS, isolated user address spaces, shell, counter and window demo. Init normally loads from `/bin/init.elf` in RAMFS; Limine modules provide a fallback.
* **Filesystem:** VFS plus RAMFS/initrd system files, and a persistent FAT16 USB data volume at `/disk` when using the new USB image. See [persistent storage](docs/PERSISTENT_STORAGE.md) for scope and testing.
* **Terminal:** TTY with line discipline, framebuffer terminal emulator (ANSI subset), shell with history, scrollback and live command highlighting.
* **Input:** xHCI USB (Intel port-routing support plus an AMD quirk workaround, hubs, connect detection), USB HID keyboard and mouse, PS/2 keyboard, Synaptics touchpad over PS/2 (see `src/drivers/SynapticsGuide.md`).
* **Platform:** ACPI (shutdown/reboot), PCI ECAM mapping and enumeration.
* **Taskbar clock:** real time and date from the CMOS RTC (`src/arch/rtc.h`), repainted when the minute changes. Windows keeps local time in the RTC, so it matches the Windows clock by default; see the `CLOCK_*` build options in the Makefile.
* **Desktop:** window manager with z-order, drag (ghost outline), close buttons, taskbar, HUD, wallpaper, mouse cursor.
* **Compositor:** surface-backed windows, a damage-rect list, and the rainbow animation as a surface client (see below).
* **Intel GPU (Gen9):** PCI discovery, MMIO + GGTT mapping, forcewake/reset, blitter (BCS) ring with fill/copy and a fence. The self-test and benchmark have run on our laptop, but the path is **off by default** (slower than the CPU copy).

### How a frame is drawn

The compositor thread (`compositor_thread_main` in `src/kernel.cpp`) runs at roughly 60 Hz on a TSC cadence. Each frame:

1. Clients update their **surfaces** and report **damage** (`wm_surface_damage*`). The Rainbow and Visual Test windows are surface clients, as are user windows. Up to 8 separate dirty rects are tracked (`src/gui/damage.h`); some writers still use one global box.
2. For each damaged rect, `wm_compose_dirty()` (`src/gui/wm.h`) builds the scene in the master backbuffer: Stage 1 wallpaper from RAM, Stage 2 all windows through the generic surface blit path, then taskbar, RAM/CPU readouts and the Start-menu overlay.
3. Stage 3 copies the rect to VRAM with the CPU (SSE non-temporal stores). An optional GPU-blitter hook exists but is only enabled if the boot benchmark shows it is faster.
4. The cursor and drag-outline overlay is drawn **directly onto VRAM, once, after the last flip** of the frame.

Conventions that are easy to get wrong: Stage 2 treats `max_y` as **exclusive**, while Stage 1 and the flip are **inclusive**. `wm_surface_damage()` converts for that reason.

**HUD (top right):** `F:4.0ms (B:1.9 W:0.9 r:0.9 L:0.9 V:0.3 X:3.0)`. F is the whole `wm_compose_dirty()`; B background; W windows; V copy to screen; r time the rainbow client spent drawing (outside F); L lock wait; X time in the frame that B/W/V do not cover. `T:` is the number of tasks. The tail shows the GPU status (`GPU:off`, `GPU:none`, ...).

### Syscalls

Convention: `rax` = number, arguments in `rdi, rsi, rdx, r10, r8`, result in `rax`.

| # | Name | Notes |
|---|---|---|
| 0 / 1 / 2 / 3 | `READ` / `WRITE` / `OPEN` / `CLOSE` | fd 0/1 are the TTY; files go through the VFS |
| 16 / 17 | `SCROLL` / `TTYRAW` | terminal scrollback, raw input mode |
| 18 / 19 / 20 | `TTYECHO` / `TTYFLUSH_INPUT` / `TTYTAKE_LAUNCH` | echo controls, discard pending input, shell consumes a Start-menu launch |
| 4 / 87 | `STAT` / `UNLINK` | file metadata/timestamps and short-name FAT16 file deletion |
| 35 / 40 | `SLEEP` / `READDIR` | timed blocking and directory enumeration |
| 39 / 60 | `GETPID` / `EXIT` | PID and retained exit status; descriptors/windows are cleaned up |
| 59 / 61 | `SPAWN` / `WAITPID` | VFS ELF launch and wait for one exact child PID |
| 88 / 169 | `SHUTDOWN` / `REBOOT` | |
| 200 | `WIN_CREATE(title, x, y, w, h)` | returns a window id; see `APIDocs/WindowAPI.md` for current problems |
| 201 | `WIN_SYNC(id)` | damages the whole surface |
| 202 | `WIN_DESTROY(id)` | |
| 203 | `WIN_GET_BUFFER(id)` | returns the mapped user surface address for user windows |
| 204 | `WIN_DAMAGE(id, x, y, w, h)` | partial update |
| 205 | `WIN_POLL_EVENT(id, event)` | nonblocking window input events |

Window authorization and ELF/user-pointer validation still need hardening. This is a prototype API, not a complete process-security boundary.

---

## Source map

| Path | What it is |
|---|---|
| `src/kernel.cpp` | `kmain`, boot sequence, compositor thread, input glue, starts `init.elf` |
| `src/gui/wm.h` | window manager and CPU compositor, HUD, taskbar, wallpaper |
| `src/gui/framebufferstuff.h`, `src/gui/damage.h`, `src/gui/surface.h`, `src/gui/boot_splash.h` | dirty tracking; damage rect list and surface helpers (pure, no kernel dependencies) |
| `src/gui/graphics.h`, `src/gui/font8x8_basic.h` | render-target abstraction, bitmap font |
| `src/core/terminal.h`, `src/core/tty.h`, `src/core/klog.h` | terminal emulator, TTY line discipline, kernel log |
| `src/core/syscall.h`, `src/core/exec.h`, `src/core/elf.h` | syscall entry/dispatch, ELF loader |
| `src/core/task.h`, `src/core/scheduler.h` | PCB, scheduler |
| `src/arch/pagingstuff.h`, `src/core/heap.h`, `src/core/structures.h`, `src/core/helpers.h`, `src/core/string.h` | memory management and utilities |
| `src/arch/gdt.h`, `src/arch/idt.h`, `src/arch/irq.h`, `src/arch/timer.h`, `src/arch/apic_timer.h`, `src/arch/acpi.h` | CPU tables, interrupts, timers, ACPI |
| `src/arch/rtc.h` | CMOS real-time clock for the taskbar clock; pure decode/format logic plus thin port I/O |
| `src/drivers/usbhelpers.h`, `src/drivers/mouse.h`, `src/drivers/ps2kb.h`, `src/drivers/synaptics.h` | xHCI/USB HID, mouse state and cursor, PS/2 keyboard, touchpad |
| `src/fs/vfs.h`, `src/fs/ramfs.h`, `src/fs/fat16.h`, `src/drivers/block.h`, `src/drivers/usb_storage.h` | VFS, RAMFS, FAT16 and USB block storage |
| `src/drivers/intel_gpu.h`, `src/drivers/gpu_cmds.h` | Intel Gen9 kernel glue; command/ring encoding (pure, no kernel dependencies) |
| `user/init.c`, `user/counter.c` | shell and console process demo, bundled in the initrd |
| `user/win_test.c` | mapped-surface/input demo, launched through the shell or Start menu |
| `tools/make_usb_image.py` | required USB image creation/refresh/FRESH-mode script |
| `APIDocs/WindowAPI.md` | window syscall documentation |
| `docs/` | handoff, GPU roadmap, app-loading design |

---

## Known gaps and issues

Details, with file pointers, are in [docs/AI_HANDOFF.md](docs/AI_HANDOFF.md). In short:

* **Apps are hard-coded.** Terminal, Visual Test, Rainbow and Boot Log are created in `wm_init`; the Start button opens a menu of `/bin/*.elf` apps and launches them on click.
* **Prototype process isolation:** per-task kernel stacks and mapped user surfaces work; ELF validation, window-ID authorization and complete address-space reclamation remain incomplete.
* **Storage and time settings:** `/bin` is still RAMFS-backed; only `/disk` persists. Clock offsets are compile-time, with no named timezone/DST settings. See the near-term plan.
* **No per-pixel alpha** anywhere in the compositor.
* **Tearing and cursor flicker are unresolved.** The cause has not been established; see the roadmap.
* **USB:** a failing device (for example a boot drive that fails descriptor reads) plugged in during the root-port scan can starve HID descriptor reads on other ports. Plug it in once the system has settled. Related wish: detect which USB device is the boot drive.

---

## Roadmap

Ordered by dependency. Each step should be bootable and testable on its own.

1. Verify the combined clock/storage/foreground-app changes on hardware.
2. Add runtime timezone settings, persistent `/bin` and desktop settings, and disk-to-RAM wallpaper loading. See [docs/NEXT_STEPS.md](docs/NEXT_STEPS.md).
3. Expand file operations, disk usage reporting, asynchronous I/O, internal-drive support and installation; continue app manifests and API hardening.
4. **CPU compositor features:** alpha, shadows, retained layers.
5. **Display control:** hardware cursor plane, then page flipping at vblank.
6. **GPU:** memory manager, interrupts, render engine, then a client API.

---

## Notes

* Recommended editor: [CodeWizard2](https://github.com/AdamJosephMather/CodeWizard2).
* The `limine/` folder should be the official *binary* branch. (We may later link our own repo instead.)
* Some build outputs and large `.raw` files are already tracked. `.gitignore` excludes local USB images, test runs and tests, but does not untrack existing binaries. Review staged source/docs separately from generated outputs.
