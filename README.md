# MyOS

A hobby 64-bit x86 operating system: C++ kernel, C userspace, booted by [Limine](https://github.com/limine-bootloader/limine).
It has a framebuffer desktop (window manager, CPU compositor, taskbar, mouse cursor), USB (xHCI) and PS/2 input,
preemptive multitasking, an ELF loader and a ring-3 shell.

*Last updated: 2026-11-02.* New here? Start with **[docs/AI_HANDOFF.md](docs/AI_HANDOFF.md)** for the current status, conventions and known issues.

The long-term goal is a **Windows-like, GPU-composited desktop**. Where we are on that road, and what comes next, is in
[docs/GPU_DESKTOP_ROADMAP.md](docs/GPU_DESKTOP_ROADMAP.md). How programs should be started (instead of being hard-coded) is in
[docs/APP_LOADING.md](docs/APP_LOADING.md).

---

## Quick start

You need WSL on Windows (Ubuntu 22.04 works well) with `make`, `clang`, `clang++`, `ld.lld`, `xorriso` and `qemu-system-x86_64`.
The Makefile calls `clang`, `clang++` and `ld.lld` directly.

**Limine:** clone the *binary* branch into the `limine/` folder:

    git clone https://github.com/limine-bootloader/limine.git --branch=v11.1.0-binary --depth=1 limine

**Build and run**

    make clean && make iso          # builds myos, init.elf and myos.iso
    make run                        # QEMU (needs OVMF.fd in the project root; it is not in the repo)

QEMU has **no Intel GPU**, so anything GPU-related (the `GPU:` boot-log lines and the HUD GPU status) can only be tested by booting
`myos.iso` on real hardware. Our test machine is an Intel Gen9 laptop (Skylake/Kaby Lake class, HD 620).

**Build options** (pass them on the build command, and run `make clean` when you change them):

| Option | Default | Meaning |
|---|---|---|
| `OPT=-O2` | `-O0` | Kernel optimisation level. `-O2` compiles and links but has **not** been run on hardware; test it on its own. |
| `GPU=1` | `0` | Run the Intel GPU bring-up, self-test and benchmark at boot (briefly paints test squares). Off by default because the CPU copy is currently faster. |

The Makefile tracks header dependencies (`.d` files), so editing a header rebuilds the kernel. It does **not** notice changes to build
options, hence the `make clean`.

---

## What works today

* **Boot:** Limine, 64-bit long mode, framebuffer, SSE enabled, on-screen kernel log (`print()` feeds a 64 KB log buffer, readable as `/bootlog.txt` and in the Boot Log window).
* **CPU/interrupts:** GDT + TSS, IDT with naked stubs, 8259 PIC remap, PIT tick, TSC-based frame pacing.
* **Memory:** Limine memory map, physical frame allocator, 4-level paging, higher-half direct map, growable kernel heap (`kmalloc`/`kfree`), per-process address spaces.
* **Tasks:** PCB, context switch, round-robin **single-core** preemptive scheduler. Other cores are detected but never started.
* **Syscalls:** `syscall`/`sysret` entry and a small dispatcher (table below).
* **Userspace:** ELF64 loader (`execve_memory`) and ring-3 transition. The only user program is the shell (`user/init.c` -> `init.elf`), loaded from a Limine module at boot.
* **Filesystem:** VFS layer plus an in-memory ramfs, currently holding `/bootlog.txt` and `/wallpaper.raw`.
* **Terminal:** TTY with line discipline, framebuffer terminal emulator (ANSI subset), shell with history, scrollback and live command highlighting.
* **Input:** xHCI USB (Intel port-routing support plus an AMD quirk workaround, hubs, connect detection), USB HID keyboard and mouse, PS/2 keyboard, Synaptics touchpad over PS/2 (see `src/SynapticsGuide.md`).
* **Platform:** ACPI (shutdown/reboot), PCI ECAM mapping and enumeration.
* **Desktop:** window manager with z-order, drag (ghost outline), close buttons, taskbar, HUD, wallpaper, mouse cursor.
* **Compositor:** surface-backed windows, a damage-rect list, and the rainbow animation as a surface client (see below).
* **Intel GPU (Gen9):** PCI discovery, MMIO + GGTT mapping, forcewake/reset, blitter (BCS) ring with fill/copy and a fence. The self-test and benchmark have run on our laptop, but the path is **off by default** (slower than the CPU copy).

### How a frame is drawn

The compositor thread (`compositor_thread_main` in `src/kernel.cpp`) runs at roughly 60 Hz on a TSC cadence. Each frame:

1. Clients update their **surfaces** and report **damage** (`wm_surface_damage*`). The Rainbow and Visual Test windows are surface clients, as are user windows. Up to 8 separate dirty rects are tracked (`src/damage.h`); some writers still use one global box.
2. For each damaged rect, `wm_compose_dirty()` (`src/wm.h`) builds the scene in the master backbuffer: Stage 1 wallpaper, Stage 2 windows back to front (chrome plus surface copy; **Terminal and Boot Log are still special-cased branches**), 2.5 taskbar, 2.6/2.7 RAM/CPU readouts.
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
| 39 / 60 | `GETPID` / `EXIT` | the exit code is currently ignored |
| 88 / 169 | `SHUTDOWN` / `REBOOT` | |
| 200 | `WIN_CREATE(title, x, y, w, h)` | returns a window id; see `APIDocs/WindowAPI.md` for current problems |
| 201 | `WIN_SYNC(id)` | damages the whole surface |
| 202 | `WIN_DESTROY(id)` | |
| 203 | `WIN_GET_BUFFER(id)` | returns the surface pointer (**see known issues**) |
| 204 | `WIN_DAMAGE(id, x, y, w, h)` | partial update |

There is **no** spawn/exec, wait, directory listing, stat or input-event syscall yet.

---

## Source map

| Path | What it is |
|---|---|
| `src/kernel.cpp` | `kmain`, boot sequence, compositor thread, input glue, starts `init.elf` |
| `src/wm.h` | window manager and CPU compositor, HUD, taskbar, wallpaper |
| `src/framebufferstuff.h`, `src/damage.h`, `src/surface.h` | dirty tracking; damage rect list and surface helpers (pure, no kernel dependencies) |
| `src/graphics.h`, `src/font8x8_basic.h` | render-target abstraction, bitmap font |
| `src/terminal.h`, `src/tty.h`, `src/klog.h` | terminal emulator, TTY line discipline, kernel log |
| `src/syscall.h`, `src/exec.h`, `src/elf.h` | syscall entry/dispatch, ELF loader |
| `src/task.h`, `src/scheduler.h` | PCB, scheduler |
| `src/pagingstuff.h`, `src/heap.h`, `src/structures.h`, `src/helpers.h`, `src/string.h` | memory management and utilities |
| `src/gdt.h`, `src/idt.h`, `src/irq.h`, `src/timer.h`, `src/acpi.h` | CPU tables, interrupts, timers, ACPI |
| `src/usbhelpers.h`, `src/mouse.h`, `src/ps2kb.h`, `src/synaptics.h` | xHCI/USB HID, mouse state and cursor, PS/2 keyboard, touchpad |
| `src/vfs.h`, `src/ramfs.h` | VFS and ramdisk |
| `src/intel_gpu.h`, `src/gpu_cmds.h` | Intel Gen9 kernel glue; command/ring encoding (pure, no kernel dependencies) |
| `user/init.c` | the shell (the only program built into the ISO) |
| `user/win_test.c` | window API demo; **not built by the Makefile** and nothing can launch it yet |
| `tools/` | image conversion scripts |
| `APIDocs/WindowAPI.md` | window syscall documentation |
| `docs/` | handoff, GPU roadmap, app-loading design |

---

## Known gaps and issues

Details, with file pointers, are in [docs/AI_HANDOFF.md](docs/AI_HANDOFF.md). In short:

* **Apps are hard-coded.** Terminal, Visual Test, Rainbow and Boot Log are created in `wm_init`; the Start button only prints "coming soon". Nothing can start a second user program.
* **Not safe for more than one user process yet:** the syscall entry path uses one global saved-RSP slot and one shared kernel stack.
* **The user window API probably cannot work from ring 3 as written** (it returns a kernel heap pointer; no ownership; no input events). It has not been exercised end to end.
* **No per-pixel alpha** anywhere in the compositor.
* **Tearing and cursor flicker are unresolved.** The cause has not been established; see the roadmap.
* **USB:** a failing device (for example a boot drive that fails descriptor reads) plugged in during the root-port scan can starve HID descriptor reads on other ports. Plug it in once the system has settled. Related wish: detect which USB device is the boot drive.

---

## Roadmap

Ordered by dependency. Each step should be bootable and testable on its own.

1. Make **every** window surface-backed (Terminal, Boot Log), then delete the special-case compositor branches.
2. **Multi-process safety, spawn/exec, app discovery** (`/apps`, manifests). See `docs/APP_LOADING.md`.
3. **Window API v2:** owner process, user-mapped surfaces, input events, cleanup on exit.
4. **CPU compositor features:** alpha, shadows, retained layers.
5. **Display control:** hardware cursor plane, then page flipping at vblank.
6. **GPU:** memory manager, interrupts, render engine, then a client API.

---

## Notes

* Recommended editor: [CodeWizard2](https://github.com/AdamJosephMather/CodeWizard2).
* The `limine/` folder should be the official *binary* branch. (We may later link our own repo instead.)
* Build artifacts (`myos`, `myos.iso`, `init.elf`, `*.o`, `iso_root/`) and the large `.raw` images are currently tracked in git; a `.gitignore` is recommended.
