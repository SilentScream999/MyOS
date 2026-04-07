#ifndef elf_h
#define elf_h

// ── elf.h ─────────────────────────────────────────────────────────────────────
//
// Phase 6 — ELF Loader & Userspace
//
// Standard definitions for purely 64-bit ELF (Executable and Linkable Format)
// binaries, according to the System V Application Binary Interface AMD64 Arch
// Supplement.
//
// ──────────────────────────────────────────────────────────────────────────────

#include <stdint.h>

#define EI_NIDENT 16

// ── ELF64 Header ──────────────────────────────────────────────────────────────

typedef struct {
    uint8_t  e_ident[EI_NIDENT]; // Magic bytes and identification info
    uint16_t e_type;             // Object file type
    uint16_t e_machine;          // Architecture
    uint32_t e_version;          // Object file version
    uint64_t e_entry;            // Entry point virtual address
    uint64_t e_phoff;            // Program header table file offset
    uint64_t e_shoff;            // Section header table file offset
    uint32_t e_flags;            // Processor-specific flags
    uint16_t e_ehsize;           // ELF header size in bytes
    uint16_t e_phentsize;        // Program header table entry size
    uint16_t e_phnum;            // Program header table entry count
    uint16_t e_shentsize;        // Section header table entry size
    uint16_t e_shnum;            // Section header table entry count
    uint16_t e_shstrndx;         // Section header string table index
} Elf64_Ehdr;

// ── ELF64 Program Header ──────────────────────────────────────────────────────

typedef struct {
    uint32_t p_type;             // Segment type
    uint32_t p_flags;            // Segment flags
    uint64_t p_offset;           // Segment file offset
    uint64_t p_vaddr;            // Segment virtual address
    uint64_t p_paddr;            // Segment physical address
    uint64_t p_filesz;           // Segment size in file
    uint64_t p_memsz;            // Segment size in memory
    uint64_t p_align;            // Segment alignment
} Elf64_Phdr;

// ── Constants ─────────────────────────────────────────────────────────────────

#define PT_LOAD    1

#define PF_X       1
#define PF_W       2
#define PF_R       4

// ── elf_check_magic ───────────────────────────────────────────────────────────

static inline bool elf_check_magic(const Elf64_Ehdr* hdr) {
    if (!hdr) return false;
    return hdr->e_ident[0] == 0x7f &&
           hdr->e_ident[1] == 'E' &&
           hdr->e_ident[2] == 'L' &&
           hdr->e_ident[3] == 'F' &&
           hdr->e_ident[4] == 2; // 64-bit class
}

#endif // elf_h
