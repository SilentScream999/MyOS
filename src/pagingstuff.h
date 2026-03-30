#ifndef paging_h
#define paging_h

#include "structures.h"
#include "helpers.h"
#include <stdint.h>

extern "C" {
	#include "../limine.h"
}

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

static uint64_t HHDM;

#define PAGE_SIZE 0x1000

static uint64_t next_free_phys_page = 0;
static uint64_t max_free_phys_page = 0;
static uint64_t max_hhdm_size = 0;

void init_physical_allocator() {
	auto memmap = memmap_req.response;
	uint64_t best_len = 0;
	uint64_t best_base = 0;

	for (uint64_t i = 0; i < memmap->entry_count; i++) {
		auto *entry = memmap->entries[i];
		if (entry->type == LIMINE_MEMMAP_USABLE && entry->length > best_len) {
			best_len = entry->length;
			best_base = (entry->base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
		}
		if (entry->length + entry->base > max_hhdm_size) {
			max_hhdm_size = entry->length + entry->base;
		}
	}

	next_free_phys_page = best_base;
	max_free_phys_page = best_base+best_len;
	
	char str[64];
	to_hex(next_free_phys_page, str);
	print((char*)"Next free phys page");
	print(str);
	
	to_hex(max_free_phys_page, str);
	print((char*)"Max free phys page");
	print(str);
}

uint64_t alloc_phys_page() {
	if (next_free_phys_page >= max_free_phys_page) {
		print((char*)"OUT OF MEMORY!");
		hcf();
	}
	
	uint64_t addr = next_free_phys_page;
	next_free_phys_page += PAGE_SIZE;
	
	return addr;
}

static inline volatile uint64_t *alloc_table(void) {
	uint64_t phys = alloc_phys_page();
	volatile uint64_t *virt = (volatile uint64_t *)(HHDM + phys);
	for (int i = 0; i < 512; i++) virt[i] = 0;
	return virt;
}

static inline volatile uint64_t *descend(volatile uint64_t *parent, size_t idx, int level) {
	uint64_t e = parent[idx];

	if (!(e & PRESENT)) {
		volatile uint64_t *child = alloc_table();
		uint64_t phys = (uint64_t)child - HHDM;
		parent[idx] = (phys & PTE_ADDR_MASK) | PRESENT | WRITABLE;
		return child;
	}

	if (level == 3 && (e & HUGE)) {
		uint64_t flags = e & ~(PTE_ADDR_MASK | HUGE);
		uint64_t base  = e & PDP_1G_ADDR_MASK;
		volatile uint64_t *pd = alloc_table();
		for (int i = 0; i < 512; i++)
			pd[i] = (base + ((uint64_t)i << 21)) | flags | PRESENT | HUGE;
		uint64_t phys = (uint64_t)pd - HHDM;
		parent[idx] = (phys & PTE_ADDR_MASK) | (flags & ~HUGE) | PRESENT | WRITABLE;
		return pd;
	}

	if (level == 2 && (e & HUGE)) {
		uint64_t flags = e & ~(PTE_ADDR_MASK | HUGE);
		uint64_t base  = e & PD_2M_ADDR_MASK;
		volatile uint64_t *pt = alloc_table();
		for (int i = 0; i < 512; i++)
			pt[i] = (base + ((uint64_t)i << 12)) | flags | PRESENT;
		uint64_t phys = (uint64_t)pt - HHDM;
		parent[idx] = (phys & PTE_ADDR_MASK) | (flags & ~HUGE) | PRESENT | WRITABLE;
		return pt;
	}

	return (volatile uint64_t *)(HHDM + (e & PTE_ADDR_MASK));
}

void map_page(uint64_t va, uint64_t pa, uint64_t flags) {
	volatile uint64_t *pml4 = (volatile uint64_t *)(HHDM + (read_cr3() & PTE_ADDR_MASK));
	volatile uint64_t *pdpt = descend(pml4, PML4_INDEX(va), 4);
	volatile uint64_t *pd   = descend(pdpt, PDPT_INDEX(va), 3);
	volatile uint64_t *pt   = descend(pd,   PD_INDEX(va),   2);
	
	pt[PT_INDEX(va)] = (pa & PTE_ADDR_MASK) | (flags | PRESENT);
	asm volatile("invlpg (%0)" :: "r"(va) : "memory");
}

void map_ecam_region(uint64_t phys_base, uint64_t virt_base, uint64_t size) {
	for (uint64_t offset = 0; offset < size; offset += 0x1000) {
		map_page(virt_base + offset, phys_base + offset, WRITABLE | CACHE_DISABLE | WRITE_THROUGH);
	}
}

void map_mmio_region(uint64_t phys_base, uint64_t virt_base, uint64_t size) {
	for (uint64_t offset = 0; offset < size; offset += 0x1000) {
		map_page(virt_base + offset, phys_base + offset, WRITABLE | CACHE_DISABLE | WRITE_THROUGH | NX);
	}
}

static void map_2m(uint64_t va, uint64_t pa, uint64_t flags) {
	volatile uint64_t *pml4 = (volatile uint64_t *)(HHDM + (read_cr3() & PTE_ADDR_MASK));
	volatile uint64_t *pdpt = descend(pml4, PML4_INDEX(va), 4);
	volatile uint64_t *pd   = descend(pdpt, PDPT_INDEX(va), 3);
	pd[PD_INDEX(va)] = (pa & PD_2M_ADDR_MASK) | (flags | PRESENT | WRITABLE | HUGE);
	asm volatile("invlpg (%0)" :: "r"(va) : "memory");
}

static void map_range_huge(uint64_t va, uint64_t pa, uint64_t len, uint64_t flags) {
	while (len >= (1ull<<21) &&
		   ((va | pa) & ((1ull<<21)-1)) == 0) {
		map_2m(va, pa, flags);
		va += (1ull<<21); pa += (1ull<<21); len -= (1ull<<21);
	}
	while (len) {
		map_page(va, pa, flags);
		va += 0x1000; pa += 0x1000; len -= 0x1000;
	}
}

static void map_hhdm_usable(uint64_t flags) {
	map_range_huge(HHDM, 0, max_hhdm_size, flags);
}

#endif