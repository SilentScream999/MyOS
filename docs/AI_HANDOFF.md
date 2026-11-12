# Project handoff

Updated 2026-11-11. This is the current working snapshot, not a change log.
Start with [README](../README.md), then the guide for the area being changed.
Pending work is in [NEXT_STEPS](NEXT_STEPS.md); commands are in [TESTING](TESTING.md).

## Current system

WyzarOS is a single-core x86-64 OS with a C++ kernel, C userspace, Limine boot,
preemptive tasks, static ELF loading and a framebuffer desktop. The long-term
target is a GPU-composited desktop with installable applications.

| Area | Implemented | Details |
|---|---|---|
| Boot | ISO/UEFI USB; framebuffer mapped before drawing; RAM recovery | [Storage](PERSISTENT_STORAGE.md) |
| Processes | Private kernel stacks/address spaces, spawn, exact-child wait, foreground cancellation | [Apps](APP_LOADING.md) |
| Desktop | Surface-backed windows, damage tracking, taskbar, Start, CPU presentation | [Display roadmap](GPU_DESKTOP_ROADMAP.md) |
| App resources | `/apps` manifests plus `/bin` discovery; `.wiz` and `.elf` use ELF contents | [Apps](APP_LOADING.md) |
| Images | PNG/JPEG wallpapers; lazy PNG icons, alpha, themes and bounded caching | [Images](IMAGES.md) |
| Fonts | Runtime TTF selection with built-in fallback; printable ASCII UI | [Fonts](FONTS.md) |
| Storage | Journaled ext4 USB root, persistent apps/settings/files, atomic saves, automatic capacity setup | [ext4](EXT4.md) |
| Input | xHCI HID, USB hubs, descriptor-based pointers, PS/2 keyboard/Synaptics | [USB/input](USB_INPUT.md) |
| GPU | Intel Gen9 blitter fill/copy/fence bring-up; opt-in | [Display roadmap](GPU_DESKTOP_ROADMAP.md) |

Normal new images contain a 256 MiB ext4 seed and occupy 385 MiB overall.
First boot can provision a larger root on a larger stick. This disk seed reserve
is separate from the recovery archive in RAM.

After the disk shell reaches its first stdin request, unused recovery RAMFS
nodes and archive/fallback-module pages are reclaimed. `/tmp`, `/run` and live
logs remain. Failed disk selection or shell loading retains recovery. Removing
the disk later offlines storage until reboot; saves never switch silently to RAM.

## Decisions and implementation constraints

- Keep `GPU=0` and kernel `OPT=-O0` as defaults. The image codec has its own
  optimized rule. Test kernel-wide optimization changes separately.
- QEMU does not emulate our Intel Gen9 GPU. Keep CPU fallbacks and bounded
  hardware waits; record what was actually tested on physical hardware.
- Apps are files plus metadata. Do not add new applications permanently to
  `wm_init`; Terminal, Boot Log, Rainbow and Visual Test remain built-in fixtures
  pending migration to ordinary applications.
- Separate parser/state logic from MMIO so host tests can exercise actual code.
- Single-core IRQ guards are not SMP locks. Storage uses a sleeping mutex plus
  short IRQ guards; do not restore interrupt-disabled runtime disk transactions.
- Renderers must not read files or decode images. Resource work publishes
  complete snapshots only after input/compositor readers finish.
- Surface rectangles have exclusive right/bottom edges; the dirty-region
  presenter takes inclusive bounds. Preserve conversions and cursor ordering.
- Kernel string helpers are local definitions, not a hosted C runtime.
  Compiler-generated libc calls and the ext4/codec shim need deliberate linkage.
- Do not remove USB protocol fixes as speculative workarounds. Preserve bounded
  reset/probe handling and exact transfer-completion ownership.
- Headers are dependency-tracked; compiler-option changes require `make clean`.
  Close QEMU before updating an image it has open.

## Known limitations

- ELF validation, segment permissions/W^X, user-pointer checks, window-ID
  authorization and full address-space cleanup remain incomplete. The app API
  is for trusted programs, not a finished security boundary.
- One foreground application per shared terminal; no process groups, general
  signals, argument/environment passing or dynamic linking.
- No translucent client windows, hardware cursor plane, page flipping, GPU
  render engine or GPU client API. PNG/font alpha works on the CPU already.
- Safe eject/reconnect, stable boot-volume identity, GPT, internal-drive drivers
  and installation remain pending. Permissions, links and UTC file timestamps
  also need defined semantics.
- Runtime timezone/DST configuration, file-transfer UI and a standalone image
  viewer are absent. Icon themes implement the native subset in [IMAGES](IMAGES.md).
- A failing device can delay sibling HID discovery during synchronous
  enumeration. Surface/ZM3310 wireless stutter is not confirmed resolved.
- Some generated binaries/images are tracked. `.gitignore` does not untrack
  them; review source/docs separately and avoid committing build outputs.
  Keep line-ending normalization separate from functional edits.

## Verification boundary

Earlier desktop/compositor behavior and the Gen9 blitter ran on the test laptop.
The owner confirmed that the discovery grace window restored Surface USB-C root
mounting, and that automatic capacity setup and `mv` worked on a physical 4 GB
stick. These observations do not validate every later driver/desktop change.

Recent host/QEMU checks cover ext4 save/recovery/failure paths, capacity setup,
USB protocol helpers, PNG/JPEG resources, lazy icon lifetimes, recovery-memory
reclamation and default-VGA ISO/USB/recovery boot. The combined build still needs
physical input, persistence, power-action and display verification. See
[TESTING](TESTING.md); do not generalize one result to an entire hardware class.

## Maintaining these docs

Update feature guides when behavior changes, this snapshot when architecture or
constraints change, and the pending-work list when work completes. Investigation
chronology belongs in version-control history. Preserve unresolved limitations
and test evidence instead of appending superseded status entries.
