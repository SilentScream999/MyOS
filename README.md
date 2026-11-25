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
| `make iso GPU=0` | Build and refresh the same image with CPU-only rendering |
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
| `GPU=0` | `1` | Disable Intel GPU probing/acceleration; unsupported hardware falls back automatically |
| `GPU_COMPOSE=0` | `GPU` | Disable opaque GPU composition and rainbow |
| `GPU_PRESENT=0` | `GPU_COMPOSE` | Use CPU presentation instead of supported vblank page flips |
| `GPU_RENDER=1` | follows `GPU_COMPOSE` | Gen9 render-engine scaling after runtime discovery and pixel readback; `0` keeps blitter composition |
| `GPU_RCS_ALPHA=0` | follows `GPU_RENDER` | Use CPU SIMD for translucency while keeping validated Intel scaling and presentation |
| `GPU_WORKLOAD=1 GPU_FRAME_DIAGNOSTICS=1` | `0` | Enable GPU performance reports, workload counts and detailed frame timings |
| `DEBUG=1` | `0` | Fixed-size USB root policy; not a compiler debug switch |
| `BOOT_DIAGNOSTICS=1` | `0` | Extra boot timings |
| `CLOCK_UTC_OFFSET_MIN=-420` | `0` | Minutes added to RTC for tray display |
| `CLOCK_SHOW_SECONDS=1` | `0` | Show seconds in the tray |
| `PERSIST_DISK=another.img` | `wyzar-usb.img` | Select the host system image |

## What works

- MP3 file playback (`play /path/song.mp3`), shared mute/volume and multimedia-key
  controls, plus analog HDA diagnostics and short stereo test tones (`audio`,
  `audio test speaker`, `audio test headphone`); ThinkPad E470 headphones are
  confirmed working, and speakers are confirmed after `audio unmute`.
  E470 firmware-mute diagnostics/unmute and deferred PS/2 multimedia keys are
  implemented; E470 music and Fn media keys are confirmed working.
  Volume hold-repeat and a taskbar slider/mute popup share the retained popup
  renderer. Startup configures the captured E470 firmware's ACPI/software-key
  path automatically. Ordinary F1–F3 remain available to applications; volume
  follows the keyboard's FnLock mapping. USB multimedia input remains future work.
  See [audio](docs/AUDIO.md).
- Limine boot, ACPI power actions, physical/page/heap allocation, per-task kernel
  stacks/address spaces and a single-core preemptive scheduler.
- Native `.wiz` packages (icons/metadata inside, portable or installed) and `.elf` programs, spawn/wait/exit, foreground Ctrl+C and mapped
  user-window surfaces/events. Both extensions currently contain WyzarOS ELFs.
- Surface-backed desktop windows, damage tracking, taskbar, Start menu and
  CPU/SSE presentation fallback. Normal windows have cached shadows and opt-in
  client transparency; title-bar text, icons and controls are cached.
  Terminals use independent userspace console hosts.
  Open more from Start or with Ctrl+Alt+T. Shift+Up/Down scrolls terminal history.
  Taskbar buttons group windows by application. Select a group to pick a window,
  or use the overflow menu for applications beyond the available taskbar space.
- Start uses the taskbar's navy gradients and bordered controls, with an All apps
  header and Restart / Shut down buttons that flush storage before the power action.
- Native File Explorer: folder navigation, file management, text previews and
  double-click app launching. Open it from Start or `run /bin/explorer.wiz`.
- Native Notepad: text editing, Save/Save As and unsaved-change prompts. Explorer
  opens text documents in it and offers a right-click Open with menu.
- Native Image Viewer: PNG/JPEG, checkerboard transparency, fit/actual size,
  zoom and pan. Open from Start or double-click an image in Explorer.
- Native Task Manager: a 60-second Core 0 CPU graph, collapsible Programs,
  Background processes and Kernel / System sections, CPU sorting within each,
  PID and scheduler state. Open with Ctrl+Shift+Esc, from Start, or
  `run /bin/task_manager.wiz`. The shortcut restores/focuses an existing window.
  Click Kernel details for an on-demand breakdown of input, USB, interrupts and
  scheduler CPU time; return to Processes to stop profiling.
- App manifests and runtime PNG icons with CPU alpha blending, theme fallback
  and demand-driven caching; PNG/JPEG wallpapers and runtime TTF fonts.
- Journaled ext4 root on USB, persistent apps/settings/files, atomic replacement,
  rename, directory operations and automatic first-boot capacity setup.
- External FAT32/exFAT drives in Explorer, progress/cancel for file transfers,
  local-clock timestamps and Eject with busy-drive checks.
- USB xHCI storage/HID/hubs, descriptor-based relative pointers, PS/2 keyboard
  and Synaptics input. Hardware coverage and limitations are in the input guide.
- Automatic Intel Gen9 probing, including visible opaque client composition
  and a resident rainbow palette. Chrome, image/font alpha and resize grips
  use CPU rendering. RCS scales clients and blends translucent windows by default
  after hardware readback checks, with CPU SIMD fallback. `GPU_RENDER=0` selects
  blitter composition.
  Client-only damage can combine BCS and
  RCS updates while retaining chrome, with CPU copies for uncovered wallpaper
  gaps and tiny resize grips. Supported hardware uses GPU copies to hidden scanout buffers
  and vblank page flips;
  unsupported firmware layouts keep CPU presentation. QEMU cannot
  validate Intel hardware or establish physical CPU savings.

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
| [App packages](docs/APP_PACKAGES.md) | Self-contained `.wiz` apps: icons, metadata, portable vs installed, `wiz` command |
| [File Explorer](docs/EXPLORER.md) | Native file browser, file operations, previews and app launching |
| [Notepad](docs/NOTEPAD.md) | Text editor, saving, shortcuts and Open with |
| [Image Viewer](docs/IMAGE_VIEWER.md) | PNG/JPEG viewing, zoom, pan and memory limits |
| [Common file dialogs](docs/FILE_DIALOGS.md) | Reusable Open/Save As browser for native apps |
| [Images/icons](docs/IMAGES.md) | Runtime resources, formats, themes, caching and memory lifetime |
| [Fonts](docs/FONTS.md) | Runtime TTF selection and limits |
| [USB/input](docs/USB_INPUT.md) | HID, hubs, reset/probe rules and hardware caveats |
| [Audio](docs/AUDIO.md) | HDA discovery, analog test tones and ThinkPad verification |
| [GPU/display roadmap](docs/GPU_DESKTOP_ROADMAP.md) | Current renderer and future display/GPU stages |
| [Syscalls](APIDocs/Syscalls.md) / [Window API](APIDocs/WindowAPI.md) | Native application interfaces |
| [Testing](docs/TESTING.md) | Host/QEMU commands and hardware verification scope |

Key gaps: ELF/user-pointer/window-ownership hardening, loader failure cleanup,
runtime timezone/DST, stable drive identity and recovery, internal-drive installation,
client present acknowledgments and hardware cursor support. Gen9 page flipping and RCS
scaling are enabled after runtime checks. Window surfaces default to opaque;
applications can opt into premultiplied source-over alpha.

## Source layout

`src/kernel.cpp` coordinates boot, presentation and input. `src/arch` contains
CPU/memory/platform code, `src/core` tasks/syscalls/TTY, `src/gui` rendering and
resources, `src/fs` VFS/RAMFS/ext4, and `src/drivers` device support. `user/`
contains the shell and example programs; `tools/` contains image/asset tooling.

Some generated outputs and large raw assets are already tracked. `.gitignore`
does not untrack them; keep generated artifacts separate from source reviews.
