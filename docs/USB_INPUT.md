# USB and pointer input

Updated 2026-11-11. Storage boot behavior is in
[PERSISTENT_STORAGE](PERSISTENT_STORAGE.md); test commands are in [TESTING](TESTING.md).

## HID pointers

`src/drivers/hid_pointer_setup.h` configures relative pointers, including composite
receivers. `logitech_g502.h` wraps the G502 HERO (`046d:c08b`). Setup records the
actual interface number/report-descriptor length, selects the pointer interface
and uses report protocol where required. Keyboard/consumer/HID++ packets are not
treated as pointer reports.

`hid_mouse_report.h` handles relative collections, optional report IDs, signed
X/Y, button usages 1–8, vertical scroll, padding/bit offsets, push/pop and
collection scope. Transfer length uses xHCI residual-byte counts. Unknown IDs
or short packets leave held buttons unchanged. Boot report byte 0 remains the
button mask. The GUI receives signed 16-bit movement and an 8-bit button mask;
larger fields are saturated. Receiver fallback remains for unsupported layouts.

DPI/RGB programming, custom bindings, horizontal scroll and auxiliary macro
reports are not implemented; onboard settings are not changed. Model-specific
hardware still needs verification after driver changes. For PS/2/Synaptics see
[the integration guide](../src/drivers/SynapticsGuide.md).

## Hubs and speeds

The driver supports USB 2 hubs, SuperSpeed hubs and the xHCI compatibility path
for SuperSpeedPlus hubs. USB-C describes a connector, not a storage protocol.

Protocol rules that must survive refactoring:

- USB 2 transaction-translator fields, including think time, follow the hub
  descriptor and actual downstream topology.
- SuperSpeed uses descriptor `0x2a`, `SET_HUB_DEPTH`, 512-byte child EP0 packets,
  nested root/route context and endpoint companion burst/interval fields.
- Decode SuperSpeed EP0 packet-size exponents. Do not interpret the USB 3 power
  bit as USB 2 low speed, or send USB 2-only port-disable requests on USB 3 failure.
- Standard speed IDs 4–7 use compatible child addressing; read the controller's
  negotiated output slot speed before configuring/recursing. Do not copy the
  upstream hub's speed to every child. Nonstandard speed IDs remain unsupported.
- USB 3 port-change acknowledgments include reset/link/configuration selectors;
  failed children enter U3. Bulk streams, UAS and Type-C/PD policy are absent.

Ports power up together and receive the descriptor's power-good delay (at least
100 ms). Root Cold Attach and inactive/compliance links use bounded warm-reset
handling; do not require CCS to appear before cold-attach recovery.

## Polling, enumeration and ownership

Root hotplug work stays local to the affected port. Do not reset a whole
controller or replace block-device objects just because one port changes.
Disconnect offlines devices under that root; old mounted handles stay offline
after reconnect. Preserve sibling HID/storage ownership.

Background hub monitoring uses asynchronous requests and bounded completion
handling. Quiet audits are staggered instead of scanning all ports every input
tick; `usb_hub_poll_policy.h` schedules audits from completion time. Initial and
actual hotplug enumeration still use bounded synchronous control requests with
a shared probe deadline. Failed controllers/ports must not enter reset storms.
Timed-out control transfers retain DMA ownership until safely recovered; do not
free their buffers simply because the caller's deadline expired.

Runtime disk I/O uses the sleeping filesystem mutex and exact controller/slot/
endpoint/TRB completion ownership. The input dispatcher can wake disk waiters
while processing HID. This does not make every discovery/control probe asynchronous.
See [EXT4](EXT4.md) for file/write semantics.

## Hardware status and diagnostics

The owner confirmed that the 1.5-second no-disk discovery grace window restored
root mounting on a Surface Laptop 4 with a ZMUIPNG ZM3310 dock. Earlier recovery
logs showed a high-speed hub (`0bda:5420`), a keyboard and no disks at selection.
Damaged unused ports were reported but were not proven to cause the failure.
Wireless keyboard stutter on that dock has not been separately confirmed fixed.

Failure-only topology/controller/PORTSC details are available in the boot log
and `/run/storage.txt` during RAM recovery. Normal successful boots suppress the
extra snapshot. A device arriving after recovery starts does not replace root.
Physical G502, wireless and SuperSpeed behavior requires its own test result;
QEMU's emulated hubs exercise USB 2 rather than physical USB 3 links.

Protocol references for further work:
[USB HID 1.11](https://www.usb.org/sites/default/files/hid1_11.pdf),
[USB 3 hub descriptor](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/usbspec/ns-usbspec-_usb_30_hub_descriptor),
[Linux hub definitions](https://github.com/torvalds/linux/blob/master/include/uapi/linux/usb/ch11.h),
[Intel xHCI 1.22](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/extensible-host-controler-interface-usb-xhci.pdf)
and the [ZM3310 manual](https://m.media-amazon.com/images/I/81%2BBUjbxAFL.pdf).
