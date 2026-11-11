# USB pointer input

Relative USB pointers, including wireless keyboard/trackpad combo receivers, use
`src/drivers/hid_pointer_setup.h`. The wired Logitech G502 HERO (`046d:c08b`) has
a model-specific wrapper in `src/drivers/logitech_g502.h`. The configuration parser records each HID
interface's report-descriptor length and actual interface number. The G502 path
reads those descriptors and selects report protocol for its pointer interface.
Pointer setup ignores non-pointer HID interfaces, so keyboard/consumer/HID++ packets cannot
be mistaken for mouse movement or button releases.

`src/drivers/hid_mouse_report.h` decodes relative mouse collections with optional
report IDs, signed X/Y, button usages 1–8 and vertical scroll. It accounts for
padding, bit offsets, global push/pop, report size/count and collection scope.
Unknown report IDs and incomplete packets leave held buttons unchanged. Input
packet length comes from the xHCI transfer's residual-byte count, rather than
assuming the entire endpoint buffer was filled. The existing GUI receives signed
16-bit movement and an 8-bit button mask; larger movement fields are saturated.

The previous boot-mouse heuristic interpreted left/right button masks as report
IDs on composite devices, shifting movement fields. Boot reports now retain byte
0 as their button mask. Numbered combo receivers explicitly select report
protocol and use declared field offsets, preserving horizontal and vertical
movement. The older receiver fallback remains when no supported descriptor layout
is available; keyboard boot reports use their existing separate path.

This is pointer input support. DPI/RGB configuration, programmable bindings,
horizontal scroll and auxiliary keyboard/macro reports are not implemented.
The driver does not modify onboard settings. Physical G502 testing is still
required; the boot log should contain:

```text
[hid] Logitech G502 HERO pointer ready (descriptor-based reports).
```

## Verification

### SuperSpeed hubs and USB-C storage

USB-C is a connector, not a distinct storage protocol. As of 2026-11-10 the hub
driver supports the USB 2 path, 5 Gbit/s SuperSpeed hubs, and the xHCI legacy
compatibility path for SuperSpeedPlus hubs.
The earlier code explicitly skipped all SuperSpeed hub children, which is a
possible explanation for the reported Surface Laptop 4 USB-C hub storage failure
while the USB-A path works. The user retested that initial change and still saw
missing storage, then confirmed the explicit SuperSpeedPlus-unsupported message.
The blanket SuperSpeedPlus rejection has now been removed; hardware retesting
of this follow-up is still needed.

The user subsequently confirmed continued failure and identified the dock as a
ZMUIPNG ZM3310, with keyboard stutter when the boot stick shares the dock. This
remains an unresolved hardware issue; neither hub identity nor stutter proves
one cause. The driver's high-speed hub TTT field now follows the descriptor's
advertised think time, including FS/LS keyboard children. On storage-root failure
only, the boot log now emits a compact discovery/topology snapshot with hub and
disk VID/PID, speed and readiness. Successful boots keep this output suppressed.
Run `python3 tests/qemu_usb_recovery_test.py` to verify this recovery-report path
on a disposable copy with an invalid root marker.
The fixture uses ISO boot to separate OS root validation from OVMF removable-
media boot selection. The ZM3310 manual recommends its USB 2.0 ports for wireless
receivers; wireless stutter alone does not establish CPU or USB event starvation.
Manufacturer manual: [ZM3310 PDF](https://m.media-amazon.com/images/I/81%2BBUjbxAFL.pdf).

The SuperSpeed path requests descriptor 0x2a, sets the hub's depth after
configuration, uses 512-byte EP0 packets for children, and preserves the complete
nested route/root-port context without USB 2 transaction-translator fields.
Hub interrupt endpoints and BOT bulk endpoints parse their SuperSpeed endpoint
companion descriptors for Max Burst and interval payload. Stream-enabled bulk
endpoints remain unsupported. Port change acknowledgments use USB 3 selectors,
including warm-reset, link-state and configuration-error changes. Failed children
enter U3 rather than sending the USB 2-only CLEAR_FEATURE(PORT_ENABLE).

For standard USB 3 speed IDs 4..7, child addressing uses the SuperSpeed-compatible
port status value. xHCI 1.22 section 4.19.9 requires SSP controllers to consume
Sublink Speed notifications and report the actual negotiated speed in the output
slot context. The driver reads that output value before configuring the child
or recursing through a nested hub. A 5-Gbit/s child behind an SSP hub remains
5-Gbit/s; the upstream speed is not blindly copied to every child. This uses the
specified compatibility path, not a new BOS/extended-port-status implementation.
Nonstandard controller speed IDs outside 1..7 remain unsupported.

All hub ports power up together and get the descriptor's power-good delay
(minimum 100 ms). Root ports with Cold Attach Status are probed even before CCS
appears and receive a warm reset; inactive/compliance SuperSpeed links also use
warm recovery. Actual reset waits remain bounded. UAS, Type-C/PD policy and
firmware boot issues remain outside this change. Root selection still
happens once before userspace, so a disk arriving after recovery starts does not
silently replace the running root.

The latest physical recovery snapshot shows two controllers, one high-speed hub
(controller 1, root port 5, reported VID/PID 0bda:5420), one keyboard and no disks
at root selection. The user also reports damaged unused dock ports. The failure
is OS discovery, before filesystem parsing; damaged ports are not a proven cause.
Boot now gives late devices a 1.5-second discovery window only if no disk is
already online, checking root ports and empty hub ports before root selection.
Individual probes retain their bounded timeouts. Run
`python3 tests/qemu_late_storage_test.py` for startup/post-recovery arrival tests.
Recovery reports include controller availability/reset state and raw root-port
PORTSC values; normal disk-root boots keep these diagnostics suppressed.
The user has now physically confirmed that this discovery window fixes root
mounting on the failing Surface/ZM3310 USB-C connection. Keyboard stutter has
not been separately confirmed resolved.

Runtime filesystem I/O now uses a reentrant sleeping mutex. Disk completion
waiters register exact controller/slot/endpoint/TRB ownership before ringing
the doorbell. The input dispatcher can deliver those completions and wake the
owner while continuing HID processing; waiters also have a timed wake/poll path.
Short IRQ guards protect event dequeue/publication and descriptor refcounts.
`tests/qemu_storage_async_test.py` injects a missing completion into an actual
write and verifies timer, keyboard, mouse and desktop presentation progress
before the bounded disk failure. Scripted actual-wait tests cover wrong/late
completions, wakeup, short/error responses, queue failure and timeout. This does
not establish that physical wireless interference or every hub hotplug delay
has been resolved; discovery/control probes still use bounded synchronous waits.

Run `python3 tests/run_usb_hub_tests.py` for sanitizer tests of the protocol
helpers, actual hub-enumeration functions with scripted completions, mixed SS/SSP
children, controller-reported output speeds, nested routes, USB 2 regression,
real root-reset loops, malformed descriptors, SET_HUB_DEPTH failure and the
asynchronous monitor's USB 2/3 acknowledgment selectors. QEMU's emulated hubs
exercise the USB 2 path, not a physical USB 3 hub. Test ext4 boot/persistence with
`python3 tests/qemu_ext4_test.py --hub-depth 1` and `--hub-depth 2` after copying
the current `wyzar-usb.img` to `.storage-test/ext4-usb.img`.

Protocol references: [SuperSpeed hub descriptor](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/usbspec/ns-usbspec-_usb_30_hub_descriptor),
[USB hub request/status definitions](https://github.com/torvalds/linux/blob/master/include/uapi/linux/usb/ch11.h).
SSP compatibility and cold attach: sections 4.19.8–4.19.9 of the
[Intel xHCI 1.22 specification](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/extensible-host-controler-interface-usb-xhci.pdf).

### Pointer input

Local regression fixtures are in the ignored `tests/` directory:

```sh
clang++ -std=c++20 -I. -fsanitize=address,undefined -no-pie tests/hid_mouse_report_test.cpp -o .storage-test/hid-mouse-test
.storage-test/hid-mouse-test
clang++ -std=c++20 -I. -Isrc/drivers -fsanitize=address,undefined -no-pie tests/g502_driver_test.cpp -o .storage-test/g502-driver-test
.storage-test/g502-driver-test
python3 tests/qemu_mouse_drag_test.py
python3 tests/qemu_root_replacement_test.py
```

Parser checks cover all 256 button masks, signed 16-bit movement, 1,000 held-drag
frames without a false release, numbered/split reports, unrelated vendor/keyboard
IDs, incomplete packets and malformed descriptors. Driver setup checks use
scripted USB control responses to verify descriptor requests, nonzero interface
numbers, report-mode selection, auxiliary filtering and failed requests. QEMU
checks ordinary mouse movement/dragging and replacement with a live disk; it
does not emulate a physical G502.

Protocol references: [USB HID 1.11](https://www.usb.org/sites/default/files/hid1_11.pdf)
and [libratbag's G502 HERO identity](https://github.com/libratbag/libratbag/blob/master/data/devices/logitech-g502-hero.device).
