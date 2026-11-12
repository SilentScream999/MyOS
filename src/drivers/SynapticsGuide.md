# Synaptics PS/2 integration

Updated 2026-11-11. The current driver is `src/drivers/synaptics.h`, included by
`src/kernel.cpp`. See [USB/input](../../docs/USB_INPUT.md) for other pointer paths.

`synaptics_init()` initializes the auxiliary PS/2 device and installs
`synaptics_irq_handler()` on IRQ12. `kmain` calls it after framebuffer mapping
and boot-timer setup, before PS/2 keyboard setup. Preserve PIC cascade IRQ2
unmasking for slave IRQs in `src/arch/irq.h`.

The handler assembles six-byte absolute packets, derives relative movement,
handles touchpad/TrackPoint buttons and routes motion through:

```cpp
extern "C" void mouse_process_input(int16_t dx, int16_t dy, uint8_t buttons);
extern "C" void mouse_process_scroll(int8_t scroll_dy);
```

Keep cursor clamping, dragging, focus and window events in that common input
path. Do not add a second desktop-specific pointer implementation to this driver.
The framebuffer is a shared `extern` pointer; it must not become a separate
header-local `static` instance. Device-specific behavior still needs physical
verification; QEMU USB pointer tests do not establish Synaptics compatibility.
