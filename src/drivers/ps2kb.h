#ifndef PS2KB_H
#define PS2KB_H

#include <stdint.h>
#include "irq.h"
#include "helpers.h"
#include "tty.h"
#include "terminal.h"
#include "wm.h"
#include "audio_hotkeys.h"
#include "fn_trace.h"

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
static uint8_t ps2_gui_held=0;
static bool ps2_clipboard_enter=false,ps2_clipboard_v=false;
static WinKeyTap ps2_win_tap;
static uint8_t ps2_pressed[2][16]{};
static unsigned ps2_pressed_count=0;
static uint32_t ps2_modifiers() {
    return (ps2_ctrl_held?WIN_MOD_CTRL:0)|(ps2_shift_held?WIN_MOD_SHIFT:0)|(ps2_alt_held?WIN_MOD_ALT:0)|
           (ps2_gui_held?WIN_MOD_GUI:0);
}
static uint32_t ps2_update_modifiers(){
    uint32_t mods=ps2_modifiers();
    g_kbd_modifiers=mods;
    wm_gui_key_update(ps2_win_tap,mods,ps2_pressed_count!=0);
    return mods;
}
static bool ps2_track_key(uint8_t scancode,bool extended){
    unsigned code=scancode&0x7f;
    uint8_t& byte=ps2_pressed[extended][code/8];
    uint8_t mask=1u<<(code%8);
    bool was_pressed=byte&mask;
    if(scancode&0x80){
        if(was_pressed){byte&=~mask;--ps2_pressed_count;}
    }else if(!was_pressed){byte|=mask;++ps2_pressed_count;}
    return was_pressed;
}

// ── IRQ1 Handler ─────────────────────────────────────────────────────────────
void ps2kb_irq_handler() {
    uint8_t status = inb(0x64);
    if (!(status & 0x01)) return; // No data
    if (status & 0x20)    return; // Data is for mouse (IRQ12)

    uint8_t scancode = inb(0x60);
    g_fn_trace.append(g_tick_count,0,scancode);

    if (scancode == 0xE0) {
        ps2_e0_prefix = true;
        return;
    }

    bool was_e0   = ps2_e0_prefix;
    ps2_e0_prefix = false;
    if(was_e0&&((scancode&0x7f)==0x5b||(scancode&0x7f)==0x5c)){
        uint8_t bit=1u<<((scancode&0x7f)-0x5b);
        if(scancode&0x80)ps2_gui_held&=~bit;else ps2_gui_held|=bit;
        ps2_update_modifiers();return;
    }
    if(scancode==0x9c)ps2_clipboard_enter=false;
    if(scancode==0xaf)ps2_clipboard_v=false;
    if(scancode==0x1c){
        if(g_clipboard_open)ps2_clipboard_enter=true;
    }

    // Shift Key tracking (Set 1: 0x2A, 0x36)
    if (scancode == 0x2A || scancode == 0x36) { if(!was_e0){ps2_shift_held=true;ps2_update_modifiers();}return; }
    if (scancode == 0xAA || scancode == 0xB6) { if(!was_e0){ps2_shift_held=false;ps2_update_modifiers();}return; }

    // Ctrl Key tracking (Set 1: 0x1D)
    if (scancode == 0x1D) { ps2_ctrl_held=true;ps2_update_modifiers();return; }
    if (scancode == 0x9D) { ps2_ctrl_held=false;ps2_update_modifiers();return; }

    // Alt Key tracking (Set 1: 0x38, E0 0x38 for right alt)
    if (scancode == 0x38) { ps2_alt_held=true;ps2_update_modifiers();return; }
    if (scancode == 0xB8) { ps2_alt_held=false;ps2_update_modifiers();return; }

    bool repeat=ps2_track_key(scancode,was_e0);
    uint32_t mods=ps2_update_modifiers();
    if(g_audio_hotkeys.accept(was_e0,scancode,repeat,g_tick_count))return;
    if(scancode==0x1c&&ps2_clipboard_enter&&!g_clipboard_open)return;
    if(scancode==0x2f){
        if(ps2_clipboard_v)return;
        ps2_clipboard_v=g_clipboard_open||mods==WIN_MOD_GUI;
    }

    // Arrow Keys (Set 1 scancodes: 0x48=Up, 0x50=Down, 0x4B=Left, 0x4D=Right)
    if (scancode == 0x48 || scancode == 0x50 || scancode == 0x4B || scancode == 0x4D) {
        uint32_t key=scancode==0x48?0x52:scancode==0x50?0x51:scancode==0x4B?0x50:0x4F;
        if(wm_clipboard_key(0,key,mods,repeat))return;
        Window* focused = wm_find_window(g_focused_win_id);
        if (focused && focused->is_system && focused->type == WIN_TYPE_TERM) {
            if (ps2_shift_held && scancode == 0x48)      term_scroll_view(-1);
            else if (ps2_shift_held && scancode == 0x50) term_scroll_view(1);
            else {
                tty_input('\x1b'); tty_input('[');
                if (scancode == 0x48)      tty_input('A');
                else if (scancode == 0x50) tty_input('B');
                else if (scancode == 0x4D) tty_input('C');
                else if (scancode == 0x4B) tty_input('D');
            }
        } else if (focused && focused->is_system && focused->type == WIN_TYPE_LOG) {
            if (scancode == 0x48) wm_log_scroll(-1);
            else if (scancode == 0x50) wm_log_scroll(1);
        } else if (focused && !focused->is_system) {
            WinEvent ev;
            ev.type = WIN_EVENT_KEY_DOWN;
            ev.x = 0; ev.y = 0;
            char arrow_ch = (scancode == 0x48 ? 'A' : (scancode == 0x50 ? 'B' : (scancode == 0x4D ? 'C' : 'D')));
            ev.key = 0x1000 + arrow_ch;
            ev.mods = mods;
            ev.delta = 0;
            wm_win_push_event(focused, ev);
        }
        return;
    }

    // Ignore other key release events (scancode > 0x80)
    if (scancode & 0x80)return;

    if(was_e0&&(scancode==0x47||scancode==0x4f||scancode==0x53)){
        if(wm_clipboard_key(0,scancode==0x53?0x4c:scancode==0x47?0x4a:0x4d,mods,repeat))return;
        Window* focused=wm_find_window(g_focused_win_id);
        if(focused&&!focused->is_system){WinEvent ev{};ev.type=WIN_EVENT_KEY_DOWN;ev.mods=mods;
            ev.key=scancode==0x47?WIN_KEY_HOME:scancode==0x4f?WIN_KEY_END:WIN_KEY_DELETE;
            wm_win_push_event(focused,ev);}return;
    }

    if (scancode < (uint8_t)sizeof(ps2_set1_map)) {
        char c = ps2_shift_held ? ps2_set1_shift_map[scancode] : ps2_set1_map[scancode];
        wm_dispatch_key(c, scancode, true, mods,repeat);
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
