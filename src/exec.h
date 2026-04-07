#ifndef exec_h
#define exec_h

// ── exec.h ────────────────────────────────────────────────────────────────────
//
// Phase 6 — ELF Loader & Userspace
//
// ──────────────────────────────────────────────────────────────────────────────

#include "elf.h"
#include "pagingstuff.h"
#include "task.h"
#include "helpers.h"
#include "syscall.h" // For enter_usermode

// ── exec_trampoline ───────────────────────────────────────────────────────────
// This is the kernel-level stub that the scheduler jumps to when it first
// schedules our new user task. It drops privileges and enters ring 3.
static void exec_trampoline() {
    if (!g_current_task || !g_current_task->user_entry) hcf();
    enter_usermode(g_current_task->user_entry, g_current_task->user_stack);
}

// ── execve_memory ─────────────────────────────────────────────────────────────
// Load an ELF executable from a memory buffer, create a new process around it,
// and return the ready Task*.

static Task* execve_memory(const uint8_t* elf_data) {
    if (!elf_data) return nullptr;

    const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)elf_data;
    if (!elf_check_magic(ehdr)) {
        print((char*)"execve: Invalid ELF magic bytes!");
        return nullptr;
    }
    
    // Create a new Address Space (PML4) isolating this process.
    uint64_t new_cr3 = create_user_page_table();
    
    // Temporarily switch to the new address space so map_page affects it directly.
    uint64_t old_cr3 = read_cr3();
    switch_to_cr3(new_cr3);
    
    // Iterate over Program Headers to map segments
    const Elf64_Phdr* phdr = (const Elf64_Phdr*)(elf_data + ehdr->e_phoff);
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            uint64_t vaddr  = phdr[i].p_vaddr;
            uint64_t memsz  = phdr[i].p_memsz;
            uint64_t filesz = phdr[i].p_filesz;
            uint64_t offset = phdr[i].p_offset;
            
            // Align start and end to 4 KiB pages
            uint64_t start_page = vaddr & ~(PAGE_SIZE - 1);
            uint64_t end_page   = (vaddr + memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            
            // Allocate and map physical pages for this segment
            for (uint64_t p = start_page; p < end_page; p += PAGE_SIZE) {
                uint64_t phys = alloc_phys_page();
                // 0x04 = USER bit, 0x02 = WRITABLE bit
                map_page(p, phys, 0x04 | 0x02);
                
                // Zero the newly allocated page
                for (int j = 0; j < (int)PAGE_SIZE; j++) ((uint8_t*)p)[j] = 0;
            }
            
            // Copy file data into the mapped segment
            uint8_t* dest = (uint8_t*)vaddr;
            const uint8_t* src = elf_data + offset;
            for (uint64_t j = 0; j < filesz; j++) dest[j] = src[j];
        }
    }
    
    // ── Allocate User Stack ───────────────────────────────────────────────────
    // We place the user stack near the top of the 128 TiB canonical user space.
    uint64_t user_stack_top    = 0x00007FFF00000000ULL;
    uint64_t user_stack_pages  = 32; // 128 KiB user stack
    uint64_t user_stack_bottom = user_stack_top - (user_stack_pages * PAGE_SIZE);
    
    for (uint64_t p = user_stack_bottom; p < user_stack_top; p += PAGE_SIZE) {
        uint64_t phys = alloc_phys_page();
        map_page(p, phys, 0x04 | 0x02);
    }
    
    // Restore the kernel's original address space
    switch_to_cr3(old_cr3);
    
    // ── Create the PCB ────────────────────────────────────────────────────────
    Task* t = task_create_user(exec_trampoline, new_cr3, 4); // 4 pages for kernel stack
    t->user_entry = ehdr->e_entry;
    t->user_stack = user_stack_top;
    
    return t;
}

#endif // exec_h
