#ifndef WYZAR_FP_CONTEXT_H
#define WYZAR_FP_CONTEXT_H

// Entry RSP must be 16-byte aligned. Keep the 512-byte FXSAVE64 image and
// 16 bytes of scratch on the interrupted task's own stack, so nested IRQs and
// sleeping syscalls retain independent state. AVX is not enabled by this OS.
// Kernel C++ starts with an empty x87 stack and default SSE control settings.
#define FP_ENTRY_SAVE(sp) \
    "subq $528, " sp "\n" \
    "fxsave64 (" sp ")\n" \
    "fninit\n" \
    "movl $0x1f80, 512(" sp ")\n" \
    "ldmxcsr 512(" sp ")\n"
#define FP_ENTRY_RESTORE(sp) \
    "fxrstor64 (" sp ")\n" \
    "addq $528, " sp "\n"

#endif
