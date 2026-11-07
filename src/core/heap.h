#ifndef heap_h
#define heap_h

// ── heap.h ────────────────────────────────────────────────────────────────────
// Kernel heap: kmalloc / kfree / krealloc
//
// DESIGN — two-level allocator:
//
//   Level 1 — Physical page pool (backing store)
//     Calls alloc_phys_page() / free_phys_page() from pagingstuff.h.
//     Pages are reachable via the HHDM linear mapping set up before init_heap()
//     is called.  The heap does NOT maintain its own page freelist — that is
//     the PMM's responsibility.
//
//   Level 2 — Variable-size block allocator (first-fit free list)
//     A doubly-linked list of HeapBlock headers threads through the heap arena.
//     kmalloc()   — first-fit scan; splits a block if there is room for a
//                   minimum-size remainder.
//     kfree()     — marks the block free and coalesces with adjacent free
//                   blocks so fragmentation stays bounded.
//     krealloc()  — in-place grow if the next block is free and large enough,
//                   otherwise alloc + copy + free.
//
// THREAD SAFETY: none yet — add a spinlock here when SMP arrives.
//
// USAGE:
//   Call init_heap() once, after init_physical_allocator() and after
//   map_hhdm_usable() (HHDM must be valid).  Then kmalloc / kfree freely.
//
// DEPENDENCIES:
//   pagingstuff.h  — alloc_phys_page(), free_phys_page(), HHDM, PAGE_SIZE
//   helpers.h      — print(), hcf()
// ──────────────────────────────────────────────────────────────────────────────

#include <stdint.h>
#include <stddef.h>
#include "pagingstuff.h"
#include "helpers.h"

// ── Configuration ──────────────────────────────────────────────────────────────

// Initial arena size in pages (4 KiB each).  1024 pages = 4 MiB.
// The heap grows automatically by HEAP_GROW_PAGES when it runs out of space.
#define HEAP_INITIAL_PAGES  1024u
#define HEAP_GROW_PAGES     256u

// Minimum allocation alignment (bytes).
// 16 bytes satisfies all x86-64 ABI requirements including SSE/AVX.
#define HEAP_ALIGN  16u

// A block is only split if the remainder is at least this large (header +
// minimum payload).  Smaller remainders are left as internal fragmentation
// rather than creating unusable slivers.
#define HEAP_MIN_SPLIT  (sizeof(HeapBlock) + HEAP_ALIGN)

// ── Block header ───────────────────────────────────────────────────────────────
// Lives immediately before each allocation's payload.
// `size` includes the header itself; payload = size - sizeof(HeapBlock).

struct HeapBlock {
    uint64_t   size;    // total bytes: header + payload (always HEAP_ALIGN multiple)
    bool       free;
    uint8_t    padding[7]; // Pad bool to 8 bytes
    HeapBlock* prev;    // previous block in address order (nullptr for first)
    HeapBlock* next;    // next block in address order (nullptr for last)
    uint64_t   magic;   // 0xDEADBEEFCAFEBABE — detects header corruption
    uint64_t   reserved; // Padding to make header 48 bytes (divisible by 16)
} __attribute__((packed));

static_assert(sizeof(HeapBlock) == 48, "HeapBlock header must be exactly 48 bytes for 16-byte alignment");

#define HEAP_MAGIC  0xDEADBEEFCAFEBABEULL

// ── Module globals ─────────────────────────────────────────────────────────────

static HeapBlock* g_heap_head       = nullptr;
static HeapBlock* g_heap_tail       = nullptr;
static uint64_t   g_heap_bytes_used = 0;   // live payload bytes (for debugging)

// ── Internal helpers ───────────────────────────────────────────────────────────

static inline uint64_t heap_align_up(uint64_t v, uint64_t align) {
    return (v + align - 1) & ~(align - 1);
}

// Validate a block header; panic on corruption.
static inline void heap_check(HeapBlock* b, const char* ctx) {
    if (b->magic != HEAP_MAGIC) {
        print((char*)"[heap] CORRUPTION in:");
        print((char*)ctx);
        hcf();
    }
}

// ── heap_grow ──────────────────────────────────────────────────────────────────
// Extend the heap arena by `pages` physical pages.
// Allocates pages one at a time from the PMM.  They are always contiguous in
// virtual address space because the entire physical RAM is linearly mapped via
// HHDM — the first page's virtual address is all we need for the block header.
// Coalesces the new block with g_heap_tail if that block is free.

static void heap_grow(uint32_t pages) {
    uint64_t bytes = (uint64_t)pages * PAGE_SIZE;

    // Allocate the first page; its HHDM address is where we put the header.
    uint64_t first_phys = alloc_phys_page();
    uint8_t* start      = (uint8_t*)(HHDM + first_phys);

    // Allocate remaining pages.  They are higher physical addresses and
    // therefore higher HHDM virtual addresses — the block header covers all
    // of them as a single contiguous virtual range.
    for (uint32_t p = 1; p < pages; p++) alloc_phys_page();

    HeapBlock* nb = (HeapBlock*)start;
    nb->size  = bytes;
    nb->free  = true;
    nb->magic = HEAP_MAGIC;
    nb->prev  = g_heap_tail;
    nb->next  = nullptr;

    if (g_heap_tail) {
        heap_check(g_heap_tail, "heap_grow coalesce");
        g_heap_tail->next = nb;

        if (g_heap_tail->free) {
            // Absorb the new block into the existing free tail.
            g_heap_tail->size += bytes;
            g_heap_tail->next  = nullptr;
            return;
        }
    } else {
        g_heap_head = nb;
    }
    g_heap_tail = nb;
}

// ── init_heap ──────────────────────────────────────────────────────────────────
// Call once after init_physical_allocator() and map_hhdm_usable().

static void init_heap() {
    heap_grow(HEAP_INITIAL_PAGES);
    print((char*)"Heap: initialised (4 MiB initial arena).");
}

// ── kmalloc ────────────────────────────────────────────────────────────────────
// Allocate `size` bytes.  Returns a kernel virtual address (via HHDM).
// Returns nullptr if size == 0.  The returned pointer is HEAP_ALIGN aligned.

static void* kmalloc(uint64_t size) {
    if (size == 0) return nullptr;

    uint64_t payload = heap_align_up(size, HEAP_ALIGN);
    uint64_t need    = payload + sizeof(HeapBlock);

    // First-fit scan.
    HeapBlock* b = g_heap_head;
    while (b) {
        heap_check(b, "kmalloc scan");
        if (b->free && b->size >= need) break;
        b = b->next;
    }

    if (!b) {
        // No suitable block — grow and retry.
        uint32_t grow = (uint32_t)((need + PAGE_SIZE - 1) / PAGE_SIZE);
        if (grow < HEAP_GROW_PAGES) grow = HEAP_GROW_PAGES;
        heap_grow(grow);
        return kmalloc(size);
    }

    // Split if the remainder would be usable.
    if (b->size >= need + HEAP_MIN_SPLIT) {
        HeapBlock* rem  = (HeapBlock*)((uint8_t*)b + need);
        rem->size  = b->size - need;
        rem->free  = true;
        rem->magic = HEAP_MAGIC;
        rem->prev  = b;
        rem->next  = b->next;

        if (b->next) {
            heap_check(b->next, "kmalloc split fix-next");
            b->next->prev = rem;
        } else {
            g_heap_tail = rem;
        }
        b->next = rem;
        b->size = need;
    }

    b->free = false;
    g_heap_bytes_used += (b->size - sizeof(HeapBlock));
    return (void*)((uint8_t*)b + sizeof(HeapBlock));
}

// ── kfree ──────────────────────────────────────────────────────────────────────
// Free a pointer returned by kmalloc or krealloc.
// Passing nullptr is a no-op (matches standard free() behaviour).

static void kfree(void* ptr) {
    if (!ptr) return;

    HeapBlock* b = (HeapBlock*)((uint8_t*)ptr - sizeof(HeapBlock));
    heap_check(b, "kfree");

    if (b->free) {
        print((char*)"[heap] DOUBLE FREE detected!");
        hcf();
    }

    g_heap_bytes_used -= (b->size - sizeof(HeapBlock));
    b->free = true;

    // Coalesce with next neighbour.
    if (b->next) {
        heap_check(b->next, "kfree coalesce-next");
        if (b->next->free) {
            HeapBlock* n = b->next;
            b->size += n->size;
            b->next  = n->next;
            if (n->next) {
                heap_check(n->next, "kfree coalesce-next fix");
                n->next->prev = b;
            } else {
                g_heap_tail = b;
            }
        }
    }

    // Coalesce with prev neighbour.
    if (b->prev) {
        heap_check(b->prev, "kfree coalesce-prev");
        if (b->prev->free) {
            HeapBlock* p = b->prev;
            p->size += b->size;
            p->next  = b->next;
            if (b->next) {
                heap_check(b->next, "kfree coalesce-prev fix");
                b->next->prev = p;
            } else {
                g_heap_tail = p;
            }
        }
    }
}

// ── krealloc ───────────────────────────────────────────────────────────────────
// Resize an allocation.  Semantics match standard realloc():
//   krealloc(nullptr, size) == kmalloc(size)
//   krealloc(ptr,    0)     == kfree(ptr), returns nullptr
//   krealloc(ptr,    size)  == resize in place if possible, else alloc+copy+free

static void* krealloc(void* ptr, uint64_t new_size) {
    if (!ptr)      return kmalloc(new_size);
    if (!new_size) { kfree(ptr); return nullptr; }

    HeapBlock* b = (HeapBlock*)((uint8_t*)ptr - sizeof(HeapBlock));
    heap_check(b, "krealloc");

    uint64_t payload     = heap_align_up(new_size, HEAP_ALIGN);
    uint64_t need        = payload + sizeof(HeapBlock);
    uint64_t old_payload = b->size - sizeof(HeapBlock);

    // Already large enough — return as-is.
    if (b->size >= need) return ptr;

    // In-place grow: absorb a free next neighbour if it is large enough.
    if (b->next && b->next->free && (b->size + b->next->size) >= need) {
        heap_check(b->next, "krealloc in-place");
        HeapBlock* n = b->next;
        b->size     += n->size;
        b->next      = n->next;
        if (n->next) {
            heap_check(n->next, "krealloc in-place fix");
            n->next->prev = b;
        } else {
            g_heap_tail = b;
        }
        g_heap_bytes_used += (b->size - sizeof(HeapBlock)) - old_payload;
        return ptr;
    }

    // Alloc + copy + free.
    void* np = kmalloc(new_size);
    if (!np) return nullptr;   // OOM — original block untouched

    uint8_t* src = (uint8_t*)ptr;
    uint8_t* dst = (uint8_t*)np;
    for (uint64_t i = 0; i < old_payload; i++) dst[i] = src[i];

    kfree(ptr);
    return np;
}

// ── heap_dump ──────────────────────────────────────────────────────────────────
// Debug helper: walk the block list and print a one-line summary per block.

static void heap_dump() {
    char str[64];
    print((char*)"── Heap dump ───────────────────────");
    HeapBlock* b = g_heap_head;
    uint32_t   n = 0;
    while (b) {
        heap_check(b, "heap_dump");
        to_hex((uint64_t)b, str); print(str);
        print(b->free ? (char*)" FREE  sz=" : (char*)" USED  sz=");
        to_str(b->size, str); print(str);
        b = b->next;
        n++;
    }
    print((char*)"Total blocks:");
    to_str(n, str); print(str);
    print((char*)"Live payload bytes:");
    to_str(g_heap_bytes_used, str); print(str);
    print((char*)"────────────────────────────────────");
}

#endif // heap_h