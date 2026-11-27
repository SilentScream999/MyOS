#ifndef WYZAR_MSR_PROBE_H
#define WYZAR_MSR_PROBE_H
// Exact recoverable RDMSR/WRMSR instructions. Unsupported registers raise #GP;
// only these kernel instructions can recover. Preserve caller IF, including
// callers already running with interrupts disabled. No userspace MSR interface.
extern "C" char power_msr_fault[], power_msr_failed[];
extern "C" char power_msr_write_fault[], power_msr_write_failed[];
extern "C" __attribute__((naked, noinline)) bool power_read_msr(uint32_t, uint64_t*) {
    __asm__ volatile(
        "pushfq\n\tcli\n\tmov %edi, %ecx\n"
        "power_msr_fault:\n\trdmsr\n\tshl $32, %rdx\n\tor %rdx, %rax\n\t"
        "mov %rax, (%rsi)\n\tmov $1, %eax\n\tpopfq\n\tret\n"
        "power_msr_failed:\n\txor %eax, %eax\n\tpopfq\n\tret\n");
}
extern "C" __attribute__((naked, noinline)) bool power_write_msr(uint32_t, uint64_t) {
    __asm__ volatile(
        "pushfq\n\tcli\n\tmov %edi, %ecx\n\tmov %rsi, %rax\n\tmov %rsi, %rdx\n\tshr $32, %rdx\n"
        "power_msr_write_fault:\n\twrmsr\n\tmov $1, %eax\n\tpopfq\n\tret\n"
        "power_msr_write_failed:\n\txor %eax, %eax\n\tpopfq\n\tret\n");
}
static bool power_msr_fixup(uint64_t vector,uint64_t error,uint64_t cs,uint64_t& rip) {
    if(vector!=13 || error || (cs&3))return false;
    if(rip==(uint64_t)power_msr_fault){rip=(uint64_t)power_msr_failed;return true;}
    if(rip==(uint64_t)power_msr_write_fault){rip=(uint64_t)power_msr_write_failed;return true;}
    return false;
}
#endif
