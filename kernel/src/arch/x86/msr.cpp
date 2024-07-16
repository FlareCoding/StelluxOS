#include "msr.h"
#include <gdt/gdt.h>
#include <interrupts/interrupts.h>
#include <paging/tlb.h>
#include <kprint.h>

// Helper functions for aligning addresses
uint64_t ALIGN_DOWN(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

uint64_t ALIGN_UP(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

int isPowerOf2(uint64_t x) {
    return (x != 0) && ((x & (x - 1)) == 0);
}

__PRIVILEGED_CODE
uint64_t readMsr(
    uint32_t msr
) {
    uint32_t low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)low | ((uint64_t)high << 32));
}

__PRIVILEGED_CODE
void writeMsr(
    uint32_t msr,
    uint64_t value
) {
    uint32_t low = value & 0xFFFFFFFF;
    uint32_t high = value >> 32;
    asm volatile("wrmsr" : : "a"(low), "d"(high), "c"(msr));
}

EXTERN_C void __asm_syscall_entry64();

__PRIVILEGED_CODE
void enableSyscallInterface() {
    uint64_t starRegValue = (((uint64_t)__TSS_PT2_SELECTOR | 3) << 48) | ((uint64_t)__KERNEL_CS << 32);

    // Setup syscall related MSRs
    writeMsr(IA32_STAR, starRegValue);
    writeMsr(IA32_LSTAR, (uint64_t)__asm_syscall_entry64);
    writeMsr(IA32_FMASK, 0x200);

    // Enable syscall instruction
    uint64_t efer = readMsr(IA32_EFER);
    efer |= IA32_EFER_SCE;
    writeMsr(IA32_EFER, efer);
}
