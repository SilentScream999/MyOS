#ifndef PS2KB_H
#define PS2KB_H

#include <stdint.h>
#include "irq.h"
#include "helpers.h"
#include "tty.h"
#include "terminal.h"
#include "wm.h"

// ── Scancode Set 1 ───────────────────────────────────────────────────────────
static const char ps2_set1_map[] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

static const char ps2_set1_shift_map[] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
};

static bool ps2_shift_held = false;
static bool ps2_ctrl_held  = false;
static bool ps2_alt_held   = false;
static bool ps2_e0_prefix  = false;

// ── IRQ1 Handler ─────────────────────────────────────────────────────────────
void ps2kb_irq_handler() {
    uint8_t status = inb(0x64);
    if (!(status & 0x01)) return; // No data
    if (status & 0x20)    return; // Data is for mouse (IRQ12)

    uint8_t scancode = inb(0x60);

    if (scancode == 0xE0) {
        ps2_e0_prefix = true;
        return;
    }

    bool was_e0   = ps2_e0_prefix;
    ps2_e0_prefix = false;

    // Shift Key tracking (Set 1: 0x2A, 0x36)
    if (scancode == 0x2A || scancode == 0x36) { ps2_shift_held = true; return; }
    if (scancode == 0xAA || scancode == 0xB6) { ps2_shift_held = false; return; }

    // Ctrl Key tracking (Set 1: 0x1D)
    if (scancode == 0x1D) { ps2_ctrl_held = true; return; }
    if (scancode == 0x9D) { ps2_ctrl_held = false; return; }

    // Alt Key tracking (Set 1: 0x38, E0 0x38 for right alt)
    if (scancode == 0x38) { ps2_alt_held = true; return; }
    if (scancode == 0xB8) { ps2_alt_held = false; return; }

    // Arrow Keys (Set 1 scancodes: 0x48=Up, 0x50=Down, 0x4B=Left, 0x4D=Right)
    if (scancode == 0x48) { // Up
        if (ps2_ctrl_held)       wm_log_scroll(-1);
        else if (ps2_shift_held) term_scroll_view(-1);
        else { tty_input('\x1b'); tty_input('['); tty_input('A'); }
        return;
    }
    if (scancode == 0x50) { // Down
        if (ps2_ctrl_held)       wm_log_scroll(1);
        else if (ps2_shift_held) term_scroll_view(1);
        else { tty_input('\x1b'); tty_input('['); tty_input('B'); }
        return;
    }
    if (scancode == 0x4B) { // Left
        tty_input('\x1b'); tty_input('['); tty_input('D');
        return;
    }
    if (scancode == 0x4D) { // Right
        tty_input('\x1b'); tty_input('['); tty_input('C');
        return;
    }

    // Ignore other key release events (scancode > 0x80)
    if (scancode & 0x80) return;

    if (scancode < (uint8_t)sizeof(ps2_set1_map)) {
        char c = ps2_shift_held ? ps2_set1_shift_map[scancode] : ps2_set1_map[scancode];
        if (c) tty_input(c);
    }
}

// ── Initialization ───────────────────────────────────────────────────────────
void ps2kb_init() {
    // Basic PS/2 Keyboard initialization: just register the IRQ.
    // The BIOS usually leaves the keyboard in a working state.
    irq_register(1, ps2kb_irq_handler);
    print((char*)"[ps2] PS/2 Keyboard handler registered on IRQ1.");
}

#endif
