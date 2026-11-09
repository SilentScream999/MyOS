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
