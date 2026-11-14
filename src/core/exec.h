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
#include "wiz.h"

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
    
    // The scheduler restores each task's recorded CR3. It must not preempt
    // this temporary address-space switch while segments are being populated.
    uint64_t mapping_flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(mapping_flags) :: "memory");

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
    if (mapping_flags & (1u << 9)) __asm__ volatile("sti" ::: "memory");
    
    // ── Create the PCB ────────────────────────────────────────────────────────
    Task* t = task_create_user(exec_trampoline, new_cr3, 4); // 4 pages for kernel stack
    t->user_entry = ehdr->e_entry;
    t->user_stack = user_stack_top;
    
    return t;
}

// ── execve_path ───────────────────────────────────────────────────────────────
// Load a .wiz or .elf app from VFS. The filename extension does not select a
// loader. A file that ends in a valid .wiz footer is a package: only its ELF
// image is read into memory (icons and data stay on disk), and its manifest
// must be well-formed and not demand a newer ABI. Anything else is a plain ELF.
static int exec_wiz_read(void* ctx, wiz_u64 offset, void* buf, wiz_u64 len) {
    file* f = (file*)ctx;
    if (!len) return 1;
    f->offset = offset;
    return vfs_read(f, buf, len) == len;
}

static bool exec_check_package(file* f, uint64_t file_size, uint64_t* load_size) {
    wiz_package pkg;
    int status = wiz_open(&pkg, exec_wiz_read, f, file_size);
    if (status == WIZ_E_NOT_WIZ) { f->offset = 0; return true; }       // ordinary ELF
    if (status == 0) {
        // Task stacks are small and this kernel builds at -O0: keep the 5 KiB of
        // manifest state on the heap.
        wiz_manifest* manifest = (wiz_manifest*)kmalloc(sizeof(wiz_manifest));
        wiz_u8* scratch = (wiz_u8*)kmalloc(WIZ_MAX_MANIFEST);
        if (!manifest || !scratch) { kfree(manifest); kfree(scratch); return false; }
        status = wiz_read_manifest_with(&pkg, manifest, scratch);
        if (status == 0) status = wiz_manifest_check_package(manifest);
        kfree(manifest); kfree(scratch);
        if (status == 0) { *load_size = pkg.footer.elf_size; f->offset = 0; return true; }
    }
    print((char*)"[exec] rejected .wiz package: ");
    print((char*)wiz_error_text(status));
    return false;
}

static Task* execve_path(const char* path) {
    if (!path) return nullptr;
    file f;
    if (vfs_open(path, 0, &f) != 0) {
        print((char*)"[exec] failed to open: ");
        print((char*)path);
        return nullptr;
    }

    uint64_t sz = f.vn->size;
    if (sz < sizeof(Elf64_Ehdr)) {
        vfs_close(&f);
        return nullptr;
    }
    if (sz >= WIZ_FOOTER_SIZE && !exec_check_package(&f, sz, &sz)) {
        vfs_close(&f);
        return nullptr;
    }
    if (sz < sizeof(Elf64_Ehdr) || sz > 32 * 1024 * 1024) {
        vfs_close(&f);
        return nullptr;
    }

    uint8_t* buf = (uint8_t*)kmalloc(sz);
    if (!buf) {
        vfs_close(&f);
        return nullptr;
    }

    uint64_t read_bytes = vfs_read(&f, buf, sz);
    vfs_close(&f);

    if (read_bytes != sz) {
        kfree(buf);
        return nullptr;
    }

    Task* t = execve_memory(buf);
    if (t) vfs_resolve_path(g_current_task ? g_current_task->cwd : "/", path,
                            t->executable, sizeof(t->executable));
    kfree(buf);
    return t;
}

// Packages declare their subsystem; legacy executables default to console.
static bool exec_is_gui(const char* path) {
    file f{};if(vfs_open(path,0,&f)!=0)return false;
    wiz_package pkg;bool gui=false;
    if(wiz_open(&pkg,exec_wiz_read,&f,f.vn->size)==0){
        auto* m=(wiz_manifest*)kmalloc(sizeof(wiz_manifest));auto* scratch=(wiz_u8*)kmalloc(WIZ_MAX_MANIFEST);
        if(m&&scratch&&wiz_read_manifest_with(&pkg,m,scratch)==0&&wiz_manifest_check_package(m)==0)
            gui=(m->seen&WIZ_KEY_TYPE)&&wiz_str_equal(m->type,"gui");
        kfree(m);kfree(scratch);
    }
    vfs_close(&f);return gui;
}
static int64_t launch_program(const char* path,bool desktop,bool* gui) {
    if(app_path_is_shell(path))return -1;
    *gui=exec_is_gui(path);bool host=desktop&&!*gui;
    Task* t=execve_path(host?"/share/system/console.elf":path);if(!t)return -1;
    t->parent_pid=(desktop||*gui)?0:g_current_task?g_current_task->pid:0;
    if(g_current_task)strcpy(t->cwd,g_current_task->cwd);
    t->gui_app=desktop||*gui;
    if(host){
        auto* c=(AppConsole*)kmalloc(sizeof(AppConsole));if(!c){task_free(t);return -1;}
        memset(c,0,sizeof(*c));c->refs=1;c->echo=true;t->console=c;t->console_host=true;
        strcpy(t->launch_target,path);
    }
    if(!desktop&&!*gui&&t->parent_pid==g_terminal_shell_pid)g_terminal_foreground_pid=t->pid;
    scheduler_add_task(t);return t->pid;
}
#endif // exec_h
