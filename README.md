# WyzarOS

A hobby x86-64 operating system with a C++ kernel, C userspace and
[Limine](https://github.com/limine-bootloader/limine) boot. It has a framebuffer
desktop, preemptive tasks, native ELF applications and persistent USB storage.
The long-term target is a Windows-like, GPU-composited desktop.

## Build and run

Use WSL/Linux with `make`, `clang`, `clang++`, `ld.lld`, `xorriso`,
`qemu-system-x86_64`, OVMF and e2fsprogs (`mke2fs`, `e2fsck`, `debugfs`).
The `limine/` directory must contain the binary release files and installer;
if missing, obtain the project's pinned release:

```sh
git clone https://github.com/limine-bootloader/limine.git --branch=v11.1.0-binary --depth=1 limine
make run
```

`make run` builds the kernel, ISO and USB image, then boots the ISO with USB
storage attached. It selects local `OVMF.fd` or installed
`/usr/share/OVMF/OVMF_CODE.fd`; override with `OVMF=/path/to/firmware.fd`.

| Command | Result |
|---|---|
| `make iso` | Build ISO and refresh USB boot/system files; preserve user data |
| `make usb-image` | Build/refresh `wyzar-usb.img` for flashing |
| `make run-usb` | Build/refresh and boot that USB image directly in QEMU |
| `make clean && make run FRESH=1` | Clean build and reset the selected system image |
| `make clean && make run FRESH=1 DEBUG=1` | Reset to a fixed 256 MiB test root |

Close QEMU before updating its image. `make clean` preserves the USB image;
`FRESH=1` resets its saved files/settings. Flash the whole `.img` for a writable
USB installation. Reflashing a physical stick replaces its contents; rebuilding
the host image does not update that stick in place.

Normal fresh images occupy 385 MiB, including a 256 MiB ext4 seed. On larger
sticks, first boot can prepare a larger root while retaining that seed.
See [storage](docs/PERSISTENT_STORAGE.md) and [copy/paste build configurations](BUILD_CONFIGS.txt).

### Build options

Repeat options on each separate `make` invocation. Clean when changing compiler
options; header dependencies are tracked automatically.

| Option | Default | Purpose |
|---|---|---|
| `OPT=-O2` | `-O0` | Kernel-wide optimization; test separately on hardware |
| `GPU=1` | `0` | Intel Gen9 bring-up/self-test/benchmark; opt-in |
| `DEBUG=1` | `0` | Fixed-size USB root policy; not a compiler debug switch |
| `BOOT_DIAGNOSTICS=1` | `0` | Extra boot timings |
| `CLOCK_UTC_OFFSET_MIN=-420` | `0` | Minutes added to RTC for tray display |
| `CLOCK_SHOW_SECONDS=1` | `0` | Show seconds in the tray |
| `PERSIST_DISK=another.img` | `wyzar-usb.img` | Select the host system image |

## What works

- Limine boot, ACPI power actions, physical/page/heap allocation, per-task kernel
  stacks/address spaces and a single-core preemptive scheduler.
- Native `.wiz`/`.elf` programs, spawn/wait/exit, foreground Ctrl+C and mapped
  user-window surfaces/events. Both extensions currently contain WyzarOS ELFs.
- Surface-backed desktop windows, damage tracking, taskbar, Start menu and
  CPU/SSE presentation. Terminal and compositor demos remain built-in fixtures.
- App manifests and runtime PNG icons with CPU alpha blending, theme fallback
  and demand-driven caching; PNG/JPEG wallpapers and runtime TTF fonts.
- Journaled ext4 root on USB, persistent apps/settings/files, atomic replacement,
  rename, directory operations and automatic first-boot capacity setup.
- USB xHCI storage/HID/hubs, descriptor-based relative pointers, PS/2 keyboard
  and Synaptics input. Hardware coverage and limitations are in the input guide.
- An opt-in Intel Gen9 blitter path. QEMU tests the CPU path, not this GPU.

The shell supports `help`, `ls`, `cd`, `pwd`, `mkdir`, `rmdir`, `df`, `write`,
`append`, `mv`, `cat`, `stat`, `rm`, `run`, `open bootlog`, `shutdown` and
`restart`. Paths can be relative. `run /bin/counter.elf` demonstrates a foreground
console app; `run /bin/win_test.elf` demonstrates mapped GUI surfaces.

## Documentation

| Guide | Purpose |
|---|---|
| [Project handoff](docs/AI_HANDOFF.md) | Current architecture, constraints, known gaps and verification boundary |
| [Pending work](docs/NEXT_STEPS.md) | Remaining platform/app/display priorities |
| [Storage](docs/PERSISTENT_STORAGE.md) | Boot layout, persistence, updates, recovery and diagnostics |
| [ext4 internals](docs/EXT4.md) | Supported profile, capacity setup and write/recovery semantics |
| [Applications](docs/APP_LOADING.md) | `.wiz`/`.elf`, manifests, launch and process lifecycle |
| [Images/icons](docs/IMAGES.md) | Runtime resources, formats, themes, caching and memory lifetime |
| [Fonts](docs/FONTS.md) | Runtime TTF selection and limits |
| [USB/input](docs/USB_INPUT.md) | HID, hubs, reset/probe rules and hardware caveats |
| [GPU/display roadmap](docs/GPU_DESKTOP_ROADMAP.md) | Current renderer and future display/GPU stages |
| [Syscalls](APIDocs/Syscalls.md) / [Window API](APIDocs/WindowAPI.md) | Native application interfaces |
| [Testing](docs/TESTING.md) | Host/QEMU commands and hardware verification scope |

Key gaps: ELF/user-pointer/window-ownership hardening, complete process memory
cleanup, runtime timezone/DST, safe eject/reconnect, internal-drive installation,
translucent client windows and hardware page flipping. PNG/font alpha already
works; general window surfaces are still composed opaque.

## Source layout

`src/kernel.cpp` coordinates boot, presentation and input. `src/arch` contains
CPU/memory/platform code, `src/core` tasks/syscalls/TTY, `src/gui` rendering and
resources, `src/fs` VFS/RAMFS/ext4, and `src/drivers` device support. `user/`
contains the shell and example programs; `tools/` contains image/asset tooling.

Some generated outputs and large raw assets are already tracked. `.gitignore`
does not untrack them; keep generated artifacts separate from source reviews.
