#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>
#include "framebufferstuff.h"

// Externs for global mouse state (defined in kernel.cpp)
extern int32_t g_mouse_x;
extern int32_t g_mouse_y;
extern uint8_t g_mouse_buttons;
extern uint8_t g_mouse_prev_buttons;

// Edge-detection helpers: returns true on the *first frame* a button is pressed
static inline bool mouse_lclick() { return (g_mouse_buttons & 1) && !(g_mouse_prev_buttons & 1); }
static inline bool mouse_rclick() { return (g_mouse_buttons & 2) && !(g_mouse_prev_buttons & 2); }
static inline bool mouse_lheld()  { return (g_mouse_buttons & 1); }
static inline bool mouse_rheld()  { return (g_mouse_buttons & 2); }

#define CURSOR_WIDTH 12
#define CURSOR_HEIGHT 19

// Classic arrow pointer with black border and white interior
// 1 = White (Interior)
// 2 = Black (Border)
// 0 = Transparent
static const uint8_t g_cursor_bitmap[CURSOR_HEIGHT][CURSOR_WIDTH] = {
    {2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {2, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {2, 1, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0},
    {2, 1, 1, 1, 2, 0, 0, 0, 0, 0, 0, 0},
    {2, 1, 1, 1, 1, 2, 0, 0, 0, 0, 0, 0},
    {2, 1, 1, 1, 1, 1, 2, 0, 0, 0, 0, 0},
    {2, 1, 1, 1, 1, 1, 1, 2, 0, 0, 0, 0},
    {2, 1, 1, 1, 1, 1, 1, 1, 2, 0, 0, 0},
    {2, 1, 1, 1, 1, 1, 1, 1, 1, 2, 0, 0},
    {2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 0},
    {2, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2},
    {2, 1, 1, 1, 2, 1, 1, 2, 0, 0, 0, 0},
    {2, 1, 2, 2, 0, 2, 1, 1, 2, 0, 0, 0},
    {2, 2, 0, 0, 0, 2, 1, 1, 2, 0, 0, 0},
    {2, 0, 0, 0, 0, 0, 2, 1, 1, 2, 0, 0},
    {0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 2, 2, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
};

static uint32_t saved_bg[CURSOR_HEIGHT][CURSOR_WIDTH];
static int32_t saved_mouse_x = -1;
static int32_t saved_mouse_y = -1;
static bool mouse_visible = false;

static inline void erase_mouse() {
    if (!fb || saved_mouse_x < 0 || !mouse_visible) return;
    for (int y = 0; y < CURSOR_HEIGHT; y++) {
        int32_t py = saved_mouse_y + y;
        if (py < 0 || py >= (int32_t)fb->height) continue;
        volatile uint32_t* row_ptr = (volatile uint32_t*)((uint8_t*)fb->address + py * fb->pitch);
        for (int x = 0; x < CURSOR_WIDTH; x++) {
            int32_t px = saved_mouse_x + x;
            if (px < 0 || px >= (int32_t)fb->width) continue;
            row_ptr[px] = saved_bg[y][x];
        }
    }
    mouse_visible = false;
}

static inline void draw_mouse() {
    if (!fb) return;
    saved_mouse_x = g_mouse_x;
    saved_mouse_y = g_mouse_y;
    for (int y = 0; y < CURSOR_HEIGHT; y++) {
        int32_t py = saved_mouse_y + y;
        if (py < 0 || py >= (int32_t)fb->height) continue;
        volatile uint32_t* row_ptr = (volatile uint32_t*)((uint8_t*)fb->address + py * fb->pitch);
        for (int x = 0; x < CURSOR_WIDTH; x++) {
            int32_t px = saved_mouse_x + x;
            if (px < 0 || px >= (int32_t)fb->width) continue;
            
            // Save BG
            saved_bg[y][x] = row_ptr[px];

            // Draw FG
            uint8_t pixel = g_cursor_bitmap[y][x];
            if (pixel == 1) {
                row_ptr[px] = 0xFFFFFFFF; // White
            } else if (pixel == 2) {
                uint32_t border_color = 0xFF000000; // Default Black
                if (g_mouse_buttons & 1) border_color = 0xFFFF0000; // Red on Left
                else if (g_mouse_buttons & 2) border_color = 0xFF00FFFF; // Cyan on Right
                row_ptr[px] = border_color;
            }
        }
    }
    mouse_visible = true;
}

static inline void update_mouse() {
    erase_mouse();
    draw_mouse();
}

#endif
