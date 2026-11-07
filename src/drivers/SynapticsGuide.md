Synaptics Trackpad Integration Guide
If you need to re-enable trackpad support later, follow these 4 essential steps:

1. The Driver (
synaptics.h
)
Keep this file in src/. It contains the initialization sequence and the IRQ12 handler.

Key Function: 
synaptics_init()
 sends the 'knock' and enables data reporting.
Key Handler: 
synaptics_irq_handler()
 reads port 0x60 and calls the unified input handler.
2. IRQ Unmasking (
irq.h
)
Ensure 
irq_register
 handles the Master PIC cascade line (IRQ2).

cpp
if (irq >= 8u) {
    outb(PIC1_DATA, (uint8_t)(inb(PIC1_DATA) & ~(1u << 2u)));
}
3. Unified Input Logic (
kernel.cpp
)
Ensure the Window Manager logic is extracted into a shared function:

cpp
extern "C" void mouse_process_input(int8_t dx, int8_t dy, uint8_t buttons);
This function handles g_mouse_x/y clamping, window dragging, and raising.

4. Boilerplate Setup
Header: Include 
synaptics.h
 in 
kernel.cpp
 (after defining the global fb).
Global Scope: Ensure extern struct limine_framebuffer *fb; is used instead of static in 
framebufferstuff.h
.
Activation: Call 
synaptics_init();
 in kmain after interrupts are enabled.