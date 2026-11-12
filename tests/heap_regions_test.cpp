#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#define paging_h
#define helpers_h
#define HEAP_HOST_TEST
#define PAGE_SIZE 4096ULL
static uint64_t HHDM = 0;
alignas(4096) static uint8_t arena[64 * 1024 * 1024];
static uint64_t cursor, singles;
static uint64_t alloc_phys_page() {
    ++singles;
    auto result = (uint64_t)(arena + cursor);
    cursor += 8192;
    return result;
}
static uint64_t alloc_phys_pages(uint64_t count) {
    auto result = (uint64_t)(arena + cursor);
    cursor += count * PAGE_SIZE;
    assert(cursor + PAGE_SIZE < sizeof(arena));
    memset(arena + cursor, 0xa5, PAGE_SIZE); // unrelated DMA/firmware-owned gap
    cursor += PAGE_SIZE;
    return result;
}
static void print(char*) {}
static void to_hex(uint64_t, char*) {}
static void to_str(uint64_t, char*) {}
[[noreturn]] static void hcf() { abort(); }
#include "../src/core/heap.h"

static void gaps_intact() {
    for (uint64_t i : {8192ULL, 20480ULL})
        for (uint64_t j = 0; j < PAGE_SIZE; ++j) assert(arena[i + j] == 0xa5);
}
int main() {
    heap_grow(2);
    assert(singles == 0 && "heap backing must be an actual contiguous allocation");
    auto* first = g_heap_head;
    heap_grow(2);
    assert(g_heap_head == first && first->size == 8192 && first->next == g_heap_tail);
    assert(g_heap_tail != first && g_heap_tail->size == 8192);
    auto* a = (uint8_t*)kmalloc(8100);
    auto* b = (uint8_t*)kmalloc(8100);
    memset(a, 0x11, 8100); memset(b, 0x22, 8100);
    kfree(b); kfree(a);
    assert(first->size == 8192 && first->next->size == 8192); // no merging across gaps
    gaps_intact();
    a = (uint8_t*)kmalloc(8100);
    memset(a, 0x33, 8100);
    auto* resized = (uint8_t*)krealloc(a, 12000);
    assert(resized != a && "realloc cannot grow through an unrelated gap");
    for (unsigned i = 0; i < 8100; ++i) assert(resized[i] == 0x33);
    memset(resized, 0x44, 12000); gaps_intact(); kfree(resized);
    // Large wallpaper-sized allocations must remain within their owned extent.
    auto* image = (uint8_t*)kmalloc(8294416);
    memset(image, 0x55, 8294416); gaps_intact(); kfree(image);
    // Adjacent split blocks inside an extent should still coalesce.
    auto* x = kmalloc(1000); auto* y = kmalloc(1000);
    kfree(y); kfree(x); gaps_intact();
    puts("heap tests passed: contiguous backing, separated extents, realloc, large assets and gap preservation");
}
