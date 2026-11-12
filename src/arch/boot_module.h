#ifndef WYZAR_BOOT_MODULE_H
#define WYZAR_BOOT_MODULE_H
#include <stdint.h>
#include "../../limine.h"

// Return only this module's owned pages, never other modules, the running kernel,
// bootloader metadata, reserved memory or pages already in the usable allocator.
// The caller must have finished all reads and serialize the physical freelist.
static uint64_t boot_module_release(limine_file* module, limine_module_response* modules,
                                    limine_memmap_response* map, uint64_t hhdm,
                                    uint64_t kernel_start, uint64_t kernel_size,
                                    void (*release_page)(uint64_t)) {
    constexpr uint64_t page_size=4096;
#if LIMINE_API_REVISION >= 2
    constexpr uint64_t module_memory=LIMINE_MEMMAP_EXECUTABLE_AND_MODULES;
#else
    constexpr uint64_t module_memory=LIMINE_MEMMAP_KERNEL_AND_MODULES;
#endif
    if (!module || !module->address || !module->size || !modules || !map || !release_page ||
        !kernel_size || kernel_start>UINT64_MAX-kernel_size || (uint64_t)module->address<hhdm) return 0;
    uint64_t start=(uint64_t)module->address-hhdm, size=module->size;
    if (start<0x100000 || start>UINT64_MAX-size || start+size>UINT64_MAX-(page_size-1)) return 0;
    uint64_t first=start&~(page_size-1), end=(start+size+page_size-1)&~(page_size-1);
    // Clear the consumed descriptor before handing any backing page to the PMM.
    module->address=nullptr; module->size=0;
    uint64_t released=0;
    for (uint64_t page=first;page<end;page+=page_size) {
        if (page<kernel_start+kernel_size && kernel_start<page+page_size) continue;
        bool owned=false, shared=false, conflicting=false;
        for (uint64_t i=0;i<map->entry_count;++i) {
            auto* region=map->entries[i];
            if (!region || region->base>UINT64_MAX-region->length) continue;
            if (region->type==module_memory && page>=region->base && page+page_size<=region->base+region->length)
                owned=true;
            if (region->type!=module_memory && page<region->base+region->length && region->base<page+page_size)
                conflicting=true;
        }
        if (!owned || conflicting) continue;
        for (uint64_t i=0;i<modules->module_count;++i) {
            auto* other=modules->modules[i];
            if (!other || other==module || !other->address || !other->size) continue;
            uint64_t address=(uint64_t)other->address;
            if (address<hhdm || address-hhdm>UINT64_MAX-other->size) { shared=true; break; }
            uint64_t base=address-hhdm;
            if (page<base+other->size && base<page+page_size) { shared=true; break; }
        }
        if (!shared) { release_page(page); released+=page_size; }
    }
    return released;
}
#endif
