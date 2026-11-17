#ifndef WYZAR_PROCESS_INFO_H
#define WYZAR_PROCESS_INFO_H
#include <stdint.h>

// SYS_PROCESS_INFO (218): one coherent snapshot, at most capacity records.
// Returns total record count so callers can indicate truncation. TSC cycles
// include syscall/interrupt work on the task; halted time is excluded.
struct ProcessInfo {
    uint32_t pid, state;
    uint64_t cpu_cycles;
    char name[256];
};
struct ProcessTotals {
    uint64_t elapsed_cycles, idle_cycles;
};
enum ProcessKind : uint32_t { PROCESS_PROGRAM,PROCESS_BACKGROUND,PROCESS_KERNEL,PROCESS_KIND_COUNT };
// Optional fourth SYS_PROCESS_INFO argument (ABI 10). Existing record/totals
// layouts stay unchanged. Kinds follow snapshot order; tick_ms timestamps it.
struct ProcessMetadata {
    uint64_t tick_ms;
    uint32_t kinds[256];
};
enum KernelCpuPhase : uint32_t {
    KCPU_OTHER, KCPU_USB_EVENTS, KCPU_HUBS, KCPU_ROOT_PORTS,
    KCPU_KEY_REPEAT, KCPU_INTERRUPTS, KCPU_SCHEDULER, KCPU_COUNT
};
// SYS_KERNEL_CPU (219): opt-in PID 0 profiling. Reading renews a two-second
// lease. No further reads automatically turn profiling off.
struct KernelCpuInfo {
    uint64_t sampled_at;
    uint64_t tick;
    uint64_t cycles[KCPU_COUNT];
    uint64_t calls[KCPU_COUNT];
};
#endif
