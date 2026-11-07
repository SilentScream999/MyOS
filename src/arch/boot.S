    .global _start

_start:

    extern kmain

    call kmain

.hang:

    hlt

    jmp .hang

