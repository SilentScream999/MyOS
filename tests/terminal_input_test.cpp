#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#define frames_h
#include "../src/gui/font8x8_basic.h"
static uint8_t* g_backbuffer;
static bool g_needs_refresh;
int32_t g_term_ox=0,g_term_oy=0;
uint32_t g_term_max_cols=16,g_term_max_rows=6;
static unsigned damage;
static void wm_dirty_chrome(){}
static void term_dirty_all(){++damage;}
static void term_dirty_rect(unsigned,unsigned,unsigned,unsigned){++damage;}
static uint32_t term_surface_stride(){return 16*8;}
static void memset_32(void* p,uint32_t value,uint64_t count){auto* pixels=(uint32_t*)p;while(count--) *pixels++=value;}
static void putp(unsigned x,unsigned y,uint32_t color){((uint32_t*)g_backbuffer)[y*term_surface_stride()+x]=color;}
// MSVC lacks the GCC atomic builtins; this fixture has one producer/thread.
#ifdef _MSC_VER
#define __sync_lock_test_and_set(p,v) (*(p)?true:(*(p)=(v),false))
#define __sync_lock_release(p) (*(p)=false)
#endif
#include "../src/core/terminal.h"
static void output(const char* text){term_enqueue_text(text,strlen(text));term_process_ring_buffer();}
int main(){
    std::vector<uint32_t> pixels(16*8*6*10);g_backbuffer=(uint8_t*)pixels.data();
    term_clear_screen();term_cursor_tick(0,true);assert(term_cursor_visible);
    unsigned before=damage;term_cursor_tick(499,true);assert(damage==before);
    term_cursor_tick(500,true);assert(!term_cursor_visible&&damage>before);
    output("abcd");assert(term_x==4&&term_cursor_visible);
    output("\x1b[2D");assert(term_x==2);output("X");assert(g_term_matrix[0][2].c=='X');
    output("\x1b[99D");assert(term_x==0&&term_y==0);
    output("1234567890123456");assert(term_x==0&&term_y==1);
    output("\x1b[D");assert(term_x==15&&term_y==0);
    output("\x1b[C");assert(term_x==0&&term_y==1);
    output("\x1b[2D\x1b[K");assert(term_x==14&&g_term_matrix[0][14].c==0&&g_term_matrix[0][15].c==0);
    term_sync_backbuffer();assert(pixels[14*8]==0xff0a0a14);
    output("\x1b[2K");for(unsigned i=0;i<16;++i)assert(!g_term_matrix[0][i].c);
    term_cursor_tick(700,false);assert(!term_cursor_visible);
    before=damage;term_cursor_tick(1700,false);assert(damage==before);
    g_needs_refresh=false;auto transitions=term_cursor_transitions;
    for(unsigned ms=1700;ms<10000;ms+=16)term_cursor_tick(ms,false);
    assert(damage==before&&!g_needs_refresh&&term_cursor_transitions==transitions);
    term_cursor_tick(1701,true);assert(term_cursor_visible);
    term_cursor_tick(2201,true);assert(!term_cursor_visible);
    output("\x1b[C");assert(term_cursor_visible); // Navigation restarts the blink.
    term_scroll_view(-1);term_cursor_tick(2300,false);assert(!term_cursor_visible);
}
