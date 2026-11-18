#ifndef WYZAR_WINDOW_INPUT_H
#define WYZAR_WINDOW_INPUT_H
#include <stdint.h>

constexpr uint32_t WIN_MOD_CTRL=1u<<0,WIN_MOD_SHIFT=1u<<1,WIN_MOD_ALT=1u<<2,WIN_MOD_GUI=1u<<3;
constexpr uint32_t WIN_MOD_TEXT_PASTE=1u<<4;

// Wait for release so the Windows key can also be used as a modifier.
struct WinKeyTap {
    bool down=false,armed=false;
    bool update(uint32_t mods,bool other_key_held){
        bool held=mods&WIN_MOD_GUI;
        bool standalone=!(mods&~WIN_MOD_GUI)&&!other_key_held;
        if(!down&&held)armed=standalone;
        bool tapped=down&&!held&&armed&&standalone;
        if(!held||!standalone)armed=false;
        down=held;
        return tapped;
    }
};
#endif
