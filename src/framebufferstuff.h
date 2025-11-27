#ifndef frames_h
#define frames_h

#include <stdint.h>
#include "font8x8_basic.h"

extern "C" {
	#include "../limine.h"
}


static struct limine_framebuffer *fb;

static inline void putp(uint32_t x, uint32_t y, uint32_t argb) {
	if (x >= fb->width || y >= fb->height) return;
	volatile uint32_t *base = (volatile uint32_t *)fb->address;
	base[y * (fb->pitch / 4) + x] = argb;
}

static inline void fill_screen_fast(uint32_t argb) {
	volatile uint8_t* base = (uint8_t*)fb->address;
	const uint32_t w = fb->width;
	for (uint32_t y = 0; y < fb->height; ++y) {
		volatile uint32_t* row = (uint32_t*)(base + y * fb->pitch);
		for (uint32_t x = 0; x < w; ++x) {
			row[x] = argb;
		}
	}
}

static void draw_char(int x, int y, char c, uint32_t color) {
	const uint8_t* glyph = font8x8_basic[(int)c];
	for (int row = 0; row < 8; row++) {
		uint8_t bits = glyph[row];
		for (int col = 0; col < 8; col++) {
			if (bits & (1 << col))
				putp(x + col, y + row, color);
		}
	}
}


static int current_print_line = 0;
static int max_print_lines = 0;

static void print(char* buffer) {
	// Calculate max lines based on screen height
	if (max_print_lines == 0) {
		max_print_lines = fb->height / 10;  // 10 pixels per line
	}
	
	// Wrap around to top if we've reached the bottom
	if (current_print_line >= max_print_lines) {
		current_print_line = 0;
		// Clear the screen
		fill_screen_fast(0x00000000);
	}
	
	uint64_t i = 0;
	while (true) {
		char c = buffer[i];
		if (c == '\0') {
			break;
		}
		draw_char(i*8, current_print_line*10, c, 0xFFFFFFFF);
		i++;
	}
	current_print_line ++;
}

#endif