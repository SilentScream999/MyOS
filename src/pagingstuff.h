#ifndef paging_h
#define paging_h

// ── pagingstuff.h ─────────────────────────────────────────────────────────────
//
// Phase 2 — Memory Management
//
// PMM  (Physical Memory Manager)
//   • Parses ALL usable Limine memory map entries (not just the largest).
//   • Bump-allocates across regions in order; when a region is exhausted it
//     advances to the next one automatically.
//   • free_phys_page() returns pages to a LIFO freelist embedded in the freed
//     pages themselves (accessed via HHDM) — zero extra memory required.
//   • alloc_phys_page() drains the freelist before touching the bump pointer.
//
// VMM  (Virtual Memory Manager)
//   • 4-level page tables (PML4 → PDPT → PD → PT).
//   • map_page()   — map one 4 KiB page with arbitrary flags.
//   • unmap_page() — clear a PTE and flush the TLB entry.
//   • map_2m() / map_range_huge() — 2 MiB huge-page fast path for HHDM setup.
//   • map_ecam_region() / map_mmio_region() — MMIO helpers (cache-disabled).
//   • map_hhdm_usable() — identity-maps all physical RAM via the HHDM offset.
//
// User / kernel address space separation
//   • USERSPACE_END   — top of the canonical user address range.
//   • KERNELSPACE_BASE — bottom of the canonical kernel address range.
//   • is_user_address() / is_kernel_address() — quick range checks.
//   • create_user_page_table() — allocate a new PML4 that shares the kernel
//     half (top 256 entries) with the current address space but has a clean
//     user half (bottom 256 entries).  Returns the physical address of the
//     new PML4 so it can be stored in a PCB and loaded with switch_to_cr3().
//   • switch_to_cr3() — write CR3, flushing the entire TLB.
//
// ─────────────────────────────────────────────────────────────────────────────

#include "structures.h"
#include "helpers.h"
#include <stdint.h>

extern "C" {
    #include "../limine.h"
}

// ── Limine requests ───────────────────────────────────────────────────────────

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_req = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_req = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_req = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

// ── Global constants / state ──────────────────────────────────────────────────

static uint64_t HHDM = 0;           // set by kmain before any alloc call
#define PAGE_SIZE 0x1000ULL

// ── Address space layout ──────────────────────────────────────────────────────
//
// x86-64 canonical address space (48-bit VA, 4-level paging):
//
//   0x0000_0000_0000_0000 – 0x0000_7FFF_FFFF_FFFF   user space   (128 TiB)
//   0xFFFF_8000_0000_0000 – 0xFFFF_FFFF_FFFF_FFFF   kernel space (128 TiB)
//
// The PML4 index boundary between the two halves is entry 256:
//   indices 0–255   → user half  (bit 47 = 0)
//   indices 256–511 → kernel half (bit 47 = 1, sign-extended)

static constexpr uint64_t USERSPACE_END    = 0x0000'8000'0000'0000ULL;
static constexpr uint64_t KERNELSPACE_BASE = 0xFFFF'8000'0000'0000ULL;

static inline bool is_user_address  (uint64_t va) { return va <  USERSPACE_END;    }
static inline bool is_kernel_address(uint64_t va) { return va >= KERNELSPACE_BASE; }

// ── PMM: physical page freelist ───────────────────────────────────────────────
// Freed pages are stored in a singly-linked list.  The first 8 bytes of each
// freed page (accessed via HHDM) hold the "next" pointer.  No external memory
// is required.

struct PhysFreeNode {
    PhysFreeNode* next;
};

static PhysFreeNode* pmm_free_head  = nullptr;
static uint64_t      pmm_free_count = 0;

// ── PMM: multi-region bump allocator ──────────────────────────────────────────
// We track every usable memory region from the Limine map and allocate from
// them in address order.  When the current region is exhausted, we advance to
// the next one.  This avoids leaving large regions unused (the old code only
// used the single largest region).

#define PMM_MAX_REGIONS 64

struct PMMRegion {
    uint64_t bump;   // next free physical address in this region
    uint64_t end;    // one-past-end physical address of this region
};

static PMMRegion pmm_regions[PMM_MAX_REGIONS];
static int       pmm_region_count   = 0;
static int       pmm_cur_region     = 0;

// max_hhdm_size is used by map_hhdm_usable() to know how much RAM to map.
static uint64_t max_hhdm_size = 0;

// ── init_physical_allocator ───────────────────────────────────────────────────
// Parse the Limine memory map.  Register every USABLE region with the bump
// allocator and compute max_hhdm_size.
// Call once, before HHDM is needed for anything except the HHDM offset itself.

void init_physical_allocator() {
    auto* memmap = memmap_req.response;
    char str[64];

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        auto* entry = memmap->entries[i];

        uint64_t top = entry->base + entry->length;
        if (top > max_hhdm_size) max_hhdm_size = top;

        if (entry->type != LIMINE_MEMMAP_USABLE) continue;
        if (pmm_region_count >= PMM_MAX_REGIONS)  continue;

        // Skip conventional memory (below 1 MiB, physical 0x0–0xFFFFF).
        //
        // Although Limine marks parts of this range as USABLE, BIOS/UEFI SMM
        // handlers continue to read and write it at runtime (IVT, BDA, EBDA,
        // BIOS shadow RAM).  If xHCI DMA targets (DCBAA, TRB rings, event ring,
        // input contexts) land here the controller reads back firmware-corrupted
        // data and returns Context State Error (code 17) on Address Device,
        // triggering an endless restart loop on every USB replug.
        //
        // Extended memory (>= 1 MiB) is exclusive to the OS after handoff.
        // The old single-region allocator implicitly used only that range by
        // always picking the largest region; this filter restores that guarantee.
        if (entry->base < 0x100000ULL) continue;

        // Align base up to page boundary.
        uint64_t base = (entry->base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);
        uint64_t end  = entry->base + entry->length;
        if (base >= end) continue;   // region too small after alignment

        pmm_regions[pmm_region_count].bump = base;
        pmm_regions[pmm_region_count].end  = end;
        pmm_region_count++;
    }

    if (pmm_region_count == 0) {
        print((char*)"PMM: no usable memory regions above 1 MiB!");
        hcf();
    }

    // Print summary.
    print((char*)"PMM: usable regions (>= 1 MiB):");
    to_str((uint64_t)pmm_region_count, str); print(str);
    to_hex(pmm_regions[0].bump, str);
    print((char*)"PMM: first region base:"); print(str);
    to_hex(max_hhdm_size, str);
    print((char*)"PMM: max_hhdm_size:"); print(str);
}

// ── alloc_phys_page ───────────────────────────────────────────────────────────
// Return the physical address of a free 4 KiB page.
// Drains the freelist before touching the bump pointer.

uint64_t alloc_phys_page() {
    // 1. Check freelist first (recycled pages).
    if (pmm_free_head) {
        PhysFreeNode* node  = pmm_free_head;
        pmm_free_head       = node->next;
        pmm_free_count--;
        uint64_t phys = (uint64_t)node - HHDM;
        // Zero the recycled page so callers see clean memory.
        uint8_t* va = (uint8_t*)node;
        for (int i = 0; i < (int)PAGE_SIZE; i++) va[i] = 0;
        return phys;
    }

    // 2. Bump from the current region; advance to next region if exhausted.
    while (pmm_cur_region < pmm_region_count) {
        PMMRegion& r = pmm_regions[pmm_cur_region];
        if (r.bump + PAGE_SIZE <= r.end) {
            uint64_t addr = r.bump;
            r.bump += PAGE_SIZE;
            return addr;
        }
        pmm_cur_region++;
    }

    print((char*)"PMM: OUT OF MEMORY!");
    hcf();
    return 0;   // unreachable
}

// ── free_phys_page ────────────────────────────────────────────────────────────
// Return a page to the PMM freelist.
// `phys` must be a PAGE_SIZE-aligned physical address previously returned by
// alloc_phys_page().  The page must not be mapped in any address space at the
// time of the call (or the caller must have unmapped it first).

void free_phys_page(uint64_t phys) {
    PhysFreeNode* node = (PhysFreeNode*)(HHDM + phys);
    node->next         = pmm_free_head;
    pmm_free_head      = node;
    pmm_free_count++;
}

// ── alloc_table ───────────────────────────────────────────────────────────────
// Allocate one zeroed page and return its VIRTUAL (HHDM) address.
// Used throughout the VMM to allocate page-table pages.

static inline volatile uint64_t* alloc_table(void) {
    uint64_t          phys = alloc_phys_page();
    volatile uint64_t* virt = (volatile uint64_t*)(HHDM + phys);
    for (int i = 0; i < 512; i++) virt[i] = 0;
    return virt;
}

// ── VMM helpers ───────────────────────────────────────────────────────────────

// descend — walk one level of the page table tree, allocating a child table
// if none exists.  Splits huge pages if a finer mapping is requested.
// We unconditionally add USER (0x04) to intermediate tables so that user mappings
// can be traversed. The final leaf PTE's USER bit dictates actual security.
static inline volatile uint64_t* descend(volatile uint64_t* parent, size_t idx, int level, uint64_t user_flag) {
    uint64_t e = parent[idx];

    if (!(e & PRESENT)) {
        volatile uint64_t* child = alloc_table();
        uint64_t phys = (uint64_t)child - HHDM;
        parent[idx] = (phys & PTE_ADDR_MASK) | PRESENT | WRITABLE | user_flag;
        return child;
    }

    // Split a 1 GiB huge page into 512 × 2 MiB pages.
    if (level == 3 && (e & HUGE)) {
        uint64_t flags = e & ~(PTE_ADDR_MASK | HUGE);
        uint64_t base  = e & PDP_1G_ADDR_MASK;
        volatile uint64_t* pd = alloc_table();
        for (int i = 0; i < 512; i++)
            pd[i] = (base + ((uint64_t)i << 21)) | flags | PRESENT | HUGE;
        uint64_t phys = (uint64_t)pd - HHDM;
        parent[idx] = (phys & PTE_ADDR_MASK) | (flags & ~HUGE) | PRESENT | WRITABLE;
        return pd;
    }

    // Split a 2 MiB huge page into 512 × 4 KiB pages.
    if (level == 2 && (e & HUGE)) {
        uint64_t flags = e & ~(PTE_ADDR_MASK | HUGE);
        uint64_t base  = e & PD_2M_ADDR_MASK;
        volatile uint64_t* pt = alloc_table();
        for (int i = 0; i < 512; i++)
            pt[i] = (base + ((uint64_t)i << 12)) | flags | PRESENT;
        uint64_t phys = (uint64_t)pt - HHDM;
        parent[idx] = (phys & PTE_ADDR_MASK) | (flags & ~HUGE) | PRESENT | WRITABLE;
        return pt;
    }

    return (volatile uint64_t*)(HHDM + (e & PTE_ADDR_MASK));
}

// ── map_page ──────────────────────────────────────────────────────────────────
// Map virtual address `va` → physical address `pa` with `flags` in the
// CURRENT address space (CR3).  PRESENT is always added.

void map_page_in(uint64_t pml4_phys, uint64_t va, uint64_t pa, uint64_t flags) {
    volatile uint64_t* pml4 = (volatile uint64_t*)(HHDM + (pml4_phys & PTE_ADDR_MASK));
    
    // Intermediate tables only get the USER bit if we are mapping a user address.
    uint64_t intermediate_user_flag = (va < KERNELSPACE_BASE) ? 0x04ULL : 0ULL;

    volatile uint64_t* pdpt = descend(pml4, PML4_INDEX(va), 4, intermediate_user_flag);
    volatile uint64_t* pd   = descend(pdpt, PDPT_INDEX(va), 3, intermediate_user_flag);
    volatile uint64_t* pt   = descend(pd,   PD_INDEX(va),   2, intermediate_user_flag);
    pt[PT_INDEX(va)] = (pa & PTE_ADDR_MASK) | (flags | PRESENT);
}

void map_page(uint64_t va, uint64_t pa, uint64_t flags) {
    map_page_in(read_cr3(), va, pa, flags);
    asm volatile("invlpg (%0)" :: "r"(va) : "memory");
}

// ── unmap_page ────────────────────────────────────────────────────────────────
// Unmap `va` from the current address space by clearing its PTE.
// Flushes the TLB entry.  Does nothing if the page was not mapped (idempotent).
// NOTE: this does NOT free the backing physical page — call free_phys_page()
// separately if the physical memory should be reclaimed.

void unmap_page(uint64_t va) {
    volatile uint64_t* pml4 = (volatile uint64_t*)(HHDM + (read_cr3() & PTE_ADDR_MASK));

    uint64_t pml4e = pml4[PML4_INDEX(va)];
    if (!(pml4e & PRESENT)) return;
    volatile uint64_t* pdpt = (volatile uint64_t*)(HHDM + (pml4e & PTE_ADDR_MASK));

    uint64_t pdpte = pdpt[PDPT_INDEX(va)];
    if (!(pdpte & PRESENT)) return;
    if (pdpte & HUGE) {
        // 1 GiB mapping — caller should not be unmapping individual pages here;
        // just clear this entry and flush.
        pdpt[PDPT_INDEX(va)] = 0;
        asm volatile("invlpg (%0)" :: "r"(va) : "memory");
        return;
    }
    volatile uint64_t* pd = (volatile uint64_t*)(HHDM + (pdpte & PTE_ADDR_MASK));

    uint64_t pde = pd[PD_INDEX(va)];
    if (!(pde & PRESENT)) return;
    if (pde & HUGE) {
        pd[PD_INDEX(va)] = 0;
        asm volatile("invlpg (%0)" :: "r"(va) : "memory");
        return;
    }
    volatile uint64_t* pt = (volatile uint64_t*)(HHDM + (pde & PTE_ADDR_MASK));

    pt[PT_INDEX(va)] = 0;
    asm volatile("invlpg (%0)" :: "r"(va) : "memory");
}

// ── MMIO / ECAM mapping helpers ───────────────────────────────────────────────

void map_ecam_region(uint64_t phys_base, uint64_t virt_base, uint64_t size) {
    for (uint64_t offset = 0; offset < size; offset += PAGE_SIZE)
        map_page(virt_base + offset, phys_base + offset,
                 WRITABLE | CACHE_DISABLE | WRITE_THROUGH);
}

void map_mmio_region(uint64_t phys_base, uint64_t virt_base, uint64_t size) {
    for (uint64_t offset = 0; offset < size; offset += PAGE_SIZE)
        map_page(virt_base + offset, phys_base + offset,
                 WRITABLE | CACHE_DISABLE | WRITE_THROUGH | NX);
}

// ── Huge-page helpers (used for HHDM identity map and EFI) ───────────────────

void map_2m(uint64_t va, uint64_t pa, uint64_t flags) {
    volatile uint64_t* pml4 = (volatile uint64_t*)(HHDM + (read_cr3() & PTE_ADDR_MASK));
    uint64_t intermediate_user_flag = (va < KERNELSPACE_BASE) ? 0x04ULL : 0ULL;
    
    volatile uint64_t* pdpt = descend(pml4, PML4_INDEX(va), 4, intermediate_user_flag);
    volatile uint64_t* pd   = descend(pdpt, PDPT_INDEX(va), 3, intermediate_user_flag);
    pd[PD_INDEX(va)] = (pa & PD_2M_ADDR_MASK) | (flags | PRESENT | WRITABLE | HUGE);
    asm volatile("invlpg (%0)" :: "r"(va) : "memory");
}

void map_range_huge(uint64_t va, uint64_t pa, uint64_t len, uint64_t flags) {
    while (len >= (1ULL << 21) && ((va | pa) & ((1ULL << 21) - 1)) == 0) {
        map_2m(va, pa, flags);
        va += (1ULL << 21); pa += (1ULL << 21); len -= (1ULL << 21);
    }
    while (len) {
        map_page(va, pa, flags);
        va += PAGE_SIZE; pa += PAGE_SIZE; len -= PAGE_SIZE;
    }
}

static void map_hhdm_usable(uint64_t flags) {
    map_range_huge(HHDM, 0, max_hhdm_size, flags);
}

// ── User / kernel address space separation ────────────────────────────────────
//
// x86-64 4-level paging divides the 48-bit VA space cleanly at PML4 entry 256:
//   entries 0–255   → user half  (canonical low addresses, bit 47 = 0)
//   entries 256–511 → kernel half (canonical high addresses, bit 47 = 1)
//
// Every process has its own PML4.  The kernel half is SHARED: all PML4s point
// to the same PDPT pages for indices 256–511, so kernel mappings (HHDM,
// drivers, heap) are visible in every address space automatically.
//
// create_user_page_table()
//   Allocates a fresh PML4.  The top 256 entries are copied from the CURRENT
//   PML4 (kernel half), giving the new space instant access to all kernel
//   mappings.  The bottom 256 entries are zeroed (clean user half).
//   Returns the PHYSICAL address of the new PML4 — store this in the PCB.
//
// switch_to_cr3(pml4_phys)
//   Writes CR3, flushing the entire TLB.  Pass the physical address returned
//   by create_user_page_table() or the kernel's own PML4 physical address.

uint64_t create_user_page_table() {
    // Allocate and zero a new PML4.
    volatile uint64_t* new_pml4 = alloc_table();
    uint64_t           new_phys  = (uint64_t)new_pml4 - HHDM;

    // Current PML4 (kernel's address space).
    volatile uint64_t* cur_pml4 = (volatile uint64_t*)(HHDM + (read_cr3() & PTE_ADDR_MASK));

    // Copy kernel half (entries 256–511) so kernel mappings are instantly
    // reachable from the new address space without any extra mapping work.
    for (int i = 256; i < 512; i++)
        new_pml4[i] = cur_pml4[i];

    // Entries 0–255 remain zero: clean user address space.

    return new_phys;
}

// switch_to_cr3 — activate an address space by writing CR3.
// Flushes the TLB completely (no PCID / ASID support yet).
static inline void switch_to_cr3(uint64_t pml4_phys) {
    asm volatile("mov %0, %%cr3" :: "r"(pml4_phys) : "memory");
}

#endif // paging_h