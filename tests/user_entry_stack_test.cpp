#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include "../src/core/elf.h"

// Model IRETQ's jump into an ordinary function. This prologue and aligned
// vector store reproduce the instruction that faulted in Image Viewer.
extern "C" __attribute__((naked, used)) void entry_alignment_probe() {
    __asm__ volatile(
        "push %rbp\n"
        "mov %rsp, %rbp\n"
        "sub $16, %rsp\n"
        "pxor %xmm0, %xmm0\n"
        "movdqa %xmm0, -16(%rbp)\n"
        "mov %rsp, %rax\n"
        "and $15, %rax\n"
        "leave\n"
        "ret\n");
}
int main() {
    constexpr uint64_t top = 0x00007fff00000000ULL;
    static_assert(elf_user_entry_stack(top) == top - 8);
    static_assert(elf_user_entry_stack(top + 7) == top - 8);
    alignas(16) uint8_t stack[4096]{};
    auto entry = elf_user_entry_stack((uint64_t)(stack + sizeof(stack)));
    assert(entry % 16 == 8 && *(uint64_t*)entry == 0);
    uint64_t alignment;
    __asm__ volatile(
        "mov %%rsp, %%rbx\n"
        "mov %[entry], %%rsp\n"
        "lea 1f(%%rip), %%rax\n"
        "mov %%rax, (%%rsp)\n"
        "jmp entry_alignment_probe\n"
        "1: mov %%rbx, %%rsp\n"
        : "=&a"(alignment) : [entry] "r"(entry)
        : "rbx", "xmm0", "memory", "cc");
    assert(alignment == 0);
    puts("User entry stack alignment and startup aligned SIMD store passed.");
}
