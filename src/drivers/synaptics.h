#ifndef SYNAPTICS_H
#define SYNAPTICS_H

#include <stdint.h>
#include "irq.h"
#include "helpers.h"

extern "C" void mouse_process_input(int16_t dx, int16_t dy, uint8_t buttons);
extern "C" void mouse_process_scroll(int8_t scroll_dy);

// ── Port Definitions ─────────────────────────────────────────────────────────
#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_COMMAND 0x64

// ── Mouse Commands ───────────────────────────────────────────────────────────
#define MOUSE_CMD_SET_RES     0xE8
#define MOUSE_CMD_STATUS      0xE9
#define MOUSE_CMD_ENABLE      0xF4
#define MOUSE_CMD_DEFAULTS    0xF6

static inline void ps2_wait_write() {
    for (int i = 0; i < 10000; i++) {
        if (!(inb(PS2_STATUS) & 2)) return;
    }
}

static inline void ps2_wait_read() {
    for (int i = 0; i < 10000; i++) {
        if (inb(PS2_STATUS) & 1) return;
    }
}

static inline void ps2_write(uint8_t data) {
    ps2_wait_write();
    outb(PS2_DATA, data);
}

static inline void ps2_command(uint8_t cmd) {
    ps2_wait_write();
    outb(PS2_COMMAND, cmd);
}

static inline void mouse_write(uint8_t data) {
    ps2_command(0xD4); // Next byte to Aux device
    ps2_write(data);
}

static inline uint8_t mouse_read() {
    ps2_wait_read();
    return inb(PS2_DATA);
}

static uint8_t packet[6];
static int packet_idx = 0;

static int32_t prev_x = -1;
static int32_t prev_y = -1;
static int32_t prev_z = 0;
static uint8_t prev_w = 255;

static uint32_t scroll_accum_y = 0;
static uint8_t pt_buttons_state = 0;
static uint8_t tp_buttons_state = 0;

// ── IRQ12 Handler ────────────────────────────────────────────────────────────
void synaptics_irq_handler() {
    uint8_t status = inb(PS2_STATUS);
    if (!(status & 1) || !(status & 0x20)) return;

    uint8_t val = inb(PS2_DATA);
    
    // Correct Synaptics Absolute Sync
    // Byte 0: Bit 7=1, Bit 6=0 -> mask 0xC0 = 0x80
    // Byte 3: Bit 7=1, Bit 6=1 -> mask 0xC0 = 0xC0
    if (packet_idx == 0 && (val & 0xC0) != 0x80) return;
    if (packet_idx == 3 && (val & 0xC0) != 0xC0) {
        packet_idx = 0;
        return;
    }

    packet[packet_idx++] = val;

    // Synaptics Absolute 6-byte Packet
    if (packet_idx == 6) {
        packet_idx = 0;

        uint8_t w = ((packet[3] & 0x04) >> 2) | ((packet[0] & 0x04) >> 1) | ((packet[0] & 0x30) >> 2);
        
        // Pass-Through packet (Guest device e.g. TrackPoint)
        if (w == 3) {
            // DO NOT read pt_buttons_state from Guest Packet[1]!
            // Lenovo Thinkpads exclusively source Trackpoint buttons from Extended Packets.
            int8_t pt_dx = packet[4];
            int8_t pt_dy = packet[5];
            
            // Middle button scroll logic
            if (pt_buttons_state & 0x04) {
                static int32_t pt_scroll_accum_y = 0;
                pt_scroll_accum_y += pt_dy;
                // Threshold of 20 allows smooth pressure-based scrolling
                if (pt_scroll_accum_y > 20) {
                    mouse_process_scroll(-1); // Up
                    pt_scroll_accum_y = 0;
                } else if (pt_scroll_accum_y < -20) {
                    mouse_process_scroll(1); // Down
                    pt_scroll_accum_y = 0;
                }
                
                // Suppress Trackpoint cursor movement while scrolling
                pt_dx = 0;
                pt_dy = 0;
            }
            
            mouse_process_input(pt_dx, -pt_dy, pt_buttons_state | tp_buttons_state);
            return;
        }

        uint8_t z = packet[2];
        int32_t x = packet[4] | ((packet[1] & 0x0F) << 8) | (((packet[3] & 0x10) >> 4) << 12);
        int32_t y = packet[5] | (((packet[1] & 0xF0) >> 4) << 8) | (((packet[3] & 0x20) >> 5) << 12);
        
        // Lenovo ThinkPads route physical Trackpoint buttons through the Synaptics controller as Extended Buttons.
        if (((packet[0] ^ packet[3]) & 0x02)) {
            uint8_t ext_left   = (packet[4] & 0x01) ? 1 : 0;
            uint8_t ext_middle = (packet[4] & 0x02) ? 1 : 0;
            uint8_t ext_right  = (packet[5] & 0x01) ? 1 : 0;
            pt_buttons_state = (ext_left ? 1 : 0) | (ext_right ? 2 : 0) | (ext_middle ? 4 : 0);
        }

        uint8_t left = ((packet[0] ^ packet[3]) & 0x01);
        static uint8_t active_clickpad_click = 0;
        
        if (left) {
            if (active_clickpad_click == 0) {
                active_clickpad_click = (w == 0) ? 0x02 : 0x01;
            }
            tp_buttons_state = active_clickpad_click;
        } else {
            active_clickpad_click = 0;
            tp_buttons_state = 0;
        }
        
        int8_t out_dx = 0;
        int8_t out_dy = 0;
        
        if (z >= 30) {
            // W=0 is Two Fingers. If dragging with Left Click (thumb on pad, index moving),
            // we MUST NOT scroll! We treat it as standard movement.
            if (w == 0 && active_clickpad_click != 0x01) {
                // Two Finger Scroll
                if (prev_z >= 30 && prev_x != -1 && prev_w == 0) {
                    int32_t dy = y - prev_y;
                    scroll_accum_y += (dy > 0 ? dy : -dy); // magnitude
                    int32_t SCROLL_THRESHOLD = 150;
                    if (scroll_accum_y > SCROLL_THRESHOLD) {
                        scroll_accum_y = 0;
                        mouse_process_scroll(dy > 0 ? -1 : 1);
                    }
                } else {
                    scroll_accum_y = 0;
                }
            } else {
                // Movement
                if (prev_z >= 30 && prev_x != -1 && prev_w != 0) {
                    int32_t dx = x - prev_x;
                    int32_t dy = y - prev_y;
                    int8_t scaled_dx = dx / 6;
                    int8_t scaled_dy = dy / 6;
                    if (scaled_dx != 0 || scaled_dy != 0) {
                        out_dx = scaled_dx;
                        out_dy = scaled_dy; 
                        x = prev_x + (scaled_dx * 6);
                        y = prev_y + (scaled_dy * 6);
                    } else {
                        x = prev_x;
                        y = prev_y;
                    }
                }
            }
        } else {
            // Touch lifted
            x = -1;
            y = -1;
            z = 0;
            scroll_accum_y = 0;
        }

        mouse_process_input(out_dx, -out_dy, pt_buttons_state | tp_buttons_state);

        prev_x = x;
        prev_y = y;
        prev_z = z;
        prev_w = w;
    }
}

static inline void syn_knock_mode(uint8_t mode) {
    uint8_t modes[4];
    modes[0] = (mode >> 6) & 3;
    modes[1] = (mode >> 4) & 3;
    modes[2] = (mode >> 2) & 3;
    modes[3] = mode & 3;

    for (int i = 0; i < 4; i++) {
        mouse_write(MOUSE_CMD_SET_RES);
        mouse_read();
        mouse_write(modes[i]);
        mouse_read();
    }
}

// ── synaptics_init ───────────────────────────────────────────────────────────
void synaptics_init() {
    // 1. Enable PS/2 Auxiliary Device
    ps2_command(0xA8);
    ps2_wait_write();

    // 2. Enable interrupts for IRQ12 in the Controller Configuration Byte
    ps2_command(0x20); // Get config
    ps2_wait_read();
    uint8_t config = inb(PS2_DATA) | 2;
    ps2_command(0x60); // Set config
    ps2_write(config);

    // 3. Absolute Mode & W-Mode Activation
    // Disabling gestures (bit 4=1) often produces cleaner raw tracking depending on model,
    // but standard mode is 0x81 (Absolute + W-mode).
    syn_knock_mode(0x81);
    
    // Commit the mode by setting sample rate to 0x14
    mouse_write(0xF3);
    mouse_read();
    mouse_write(0x14);
    mouse_read();

    // 4. Configure & Enable
    // MOUSE_CMD_DEFAULTS (0xF6) resets the device to standard PS/2 mode, negating our knock!
    // We only need to enable reporting now.
    mouse_write(MOUSE_CMD_ENABLE);
    mouse_read();

    irq_register(12, synaptics_irq_handler);
    print((char*)"[syn] Synaptics Absolute Multi-Touch initialized.");
}


#endif
