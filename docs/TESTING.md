# Verification guide

Updated 2026-11-11. Run commands from the repository root in WSL/Linux.
Use current build artifacts, clang sanitizers, QEMU, OVMF and e2fsprogs as needed.
Build/setup instructions are in [README](../README.md).

Run QEMU suites sequentially: the shared harness uses fixed QMP/debug ports.
Keep physical-device tests separate from emulator results. Fixture scripts can
create large private images under `.storage-test`; retain host staging space and
do not use a live/personal system image as a disposable fixture.

## Boot, images and recovery lifetimes

```sh
make iso
python3 tests/run_image_tests.py
python3 tests/run_wiz_tests.py        # or: make test-wiz (.wiz format, desktop, shell, app library)
python3 tests/qemu_boot_framebuffer_test.py
python3 tools/make_usb_image.py --output .storage-test/image-runtime.img --debug
python3 tests/qemu_image_test.py .storage-test/image-runtime.img
```

For an existing `image-runtime.img`, use `--update-boot --debug` rather than
creating it again. The host suite covers PNG variants/Adam7/JPEG, alpha/resize/
clipping, malformed input, allocation failures, manifests/themes, lazy resource
I/O, cache replacement/eviction and safe recovery-tree/module-page reclamation.
QEMU covers default-VGA ISO boot, USB boot, RAM recovery, live icon/manifest
replacement, menu/window resource lifetimes and failed-disk-shell fallback.

## Native File Explorer

```sh
make test-explorer
make test-console
make iso PERSIST_DISK=.storage-test/explorer-seed.img ROOT_MIB=64
python3 tests/qemu_explorer_test.py
```

Host tests cover the actual userspace file operations and input handling with
ASan/UBSan. The QEMU test uses real pointer/keyboard events, validates `TRYWAIT`,
Start launch, browsing, package launching, file dialogs and persistence, then
checks the resulting ext4 filesystem. See [Explorer](EXPLORER.md) for limits.

## CPU usage and terminal wakeups

```sh
clang++ -std=c++20 -O1 -g -no-pie -fsanitize=address,undefined tests/tty_idle_test.cpp -o .storage-test/tty-idle-test
.storage-test/tty-idle-test
python3 tests/qemu_cpu_idle_test.py
```

The host test covers idle intervals interrupted by scheduled work, sample
initialization and raw/canonical/launch/cancellation wakeups. QEMU verifies that
an untouched prompt blocks, Counter returns to the same sleeping prompt, real
input and Start wake the shell, and a busy loop reports busy CPU time. The HUD
includes desktop/driver work; an idle prompt does not imply a zero-percent OS.

For rainbow animation visibility and repaint cost:

```sh
clang++ -std=c++20 -O1 -g -no-pie -fsanitize=address,undefined tests/visibility_test.cpp -o .storage-test/visibility-test
.storage-test/visibility-test
python3 tools/make_usb_image.py --output .storage-test/rainbow-base.img --debug
python3 tests/qemu_rainbow_test.py
```

Refresh an existing fixture with `--update-boot --debug`. Host checks compare
exposed coverage with a pixel mask, including clipping and bounded-region
overflow. QEMU drags actual windows, verifies fully covered animation stops and
resumes on exposure, and records CPU/frame costs and screenshots. Timings depend
on the host; this does not measure physical GPU performance.

## Storage and capacity

Prepare the dedicated ext4 fixture before its dependent tests:

```sh
python3 tools/make_usb_image.py --output .storage-test/ext4-usb.img --fresh
python3 tests/run_ext4_tests.py
python3 tests/qemu_ext4_test.py --hub-depth 2
```

The ext4 runner exercises the real adapter with ASan/UBSan, failure containment,
cold journal recovery, atomic package exchanges at 24 injected write boundaries,
and Linux filesystem checking. The QEMU suite writes its
fixture across repeated boots, including unclean poweroff and normal shutdown.

For package-specific checks, use a freshly built fixture containing the sample
packages (the QEMU test boots its own disposable copy):

```sh
make all
make test-wiz
python3 tools/make_usb_image.py --output .storage-test/ext4-usb.img --update-boot
python3 tests/qemu_wiz_packages_test.py
```

The host suite covers checksum detection throughout the executable and payload,
legacy v1 reading, first-entry duplicate semantics, failed upgrade publication,
57/64-character IDs, desktop discovery/icons and the bundle library. QEMU checks
portable execution, install/reinstall through the actual exchange syscall,
inspection, pinning, removal, purge and preservation of saved app data.

| Command | Coverage |
|---|---|
| `python3 tests/ext4_image_test.py` | Creation, update preservation, invalid roots/features, atomic publication |
| `python3 tests/atomic_save_test.py` | Injected save failures and complete old/new destinations |
| `python3 tests/capacity_image_test.py` | Offline image expansion, preservation and bounded host memory |
| `python3 tests/run_autogrow_tests.py` | First-boot format/copy/publication failure and retry, policy/UUID checks |
| `python3 tests/qemu_autogrow_test.py` | Automatic larger root and fixed DEBUG policy across boots |
| `python3 tests/qemu_storage_api_test.py` | Actual save/file syscalls and cleanup |
| `python3 tests/qemu_storage_async_test.py` | Input/timer/desktop progress during a missing disk completion |
| `python3 tests/qemu_power_action_test.py` | Shutdown/restart with connected and unplugged boot storage |

## USB and fonts

| Command | Coverage |
|---|---|
| `python3 tests/run_usb_hub_tests.py` | Protocol helpers, scripted actual enumeration/reset/wait/write paths, completion ownership |
| `python3 tests/qemu_late_storage_test.py` | Discovery grace and fixed root after recovery starts |
| `python3 tests/qemu_usb_recovery_test.py` | Root-failure report and topology diagnostics |
| `python3 tests/qemu_font_selection_test.py wyzar-usb.img assets/fonts/selawik/selawk.ttf` | Custom/missing/corrupt TTF selection and recovery font |

Other pointer/drag/RTC fixtures may exist locally but are not all tracked. Do not
make them prerequisites for a fresh checkout. See [USB_INPUT](USB_INPUT.md) and
[FONTS](FONTS.md) for the corresponding implementation boundaries.

## Physical evidence

The owner has reported earlier desktop/Gen9 blitter operation, restored Surface
USB-C root mounting, and automatic capacity setup/`mv` on a 4 GB stick. Current
full-build input, storage latency, wireless stutter, power actions and display
performance still need targeted hardware checks. QEMU does not test Intel GPU
registers or physical USB 3 links. Kernel-wide `OPT=-O2` runtime verification is
separate from the optimized image codec.

For failures, retain `/bootlog.txt`, recovery `/run/storage.txt` and relevant
QEMU stop/register state or slow-motion display footage. A GUI Paused state can
mean a guest triple fault/shutdown; do not assume it is an ordinary OS wait.
Tests demonstrate the listed cases, not every power-cut point or hardware model.
