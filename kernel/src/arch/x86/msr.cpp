#include "msr.h"
#include <gdt/gdt.h>
#include <interrupts/interrupts.h>
#include <paging/tlb.h>

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

__PRIVILEGED_CODE
void disableCpuCache() {
    unsigned long long cr0 = 0;
    asm volatile (
        "mov %%cr0, %0\n\t"        // Move CR0 into our local variable
        "or $0x40000000, %0\n\t"   // Set CD bit (bit 30)
        "or $0x20000000, %0\n\t"   // Set NW bit (bit 29) to be consistent
        "mov %0, %%cr0\n\t"        // Move our local variable back into CR0
        "wbinvd\n\t"               // Flush caches
        : "=r"(cr0)                // Output to local variable
        : "0"(cr0)                 // Input from the same local variable
        : "memory"
    );
}

__PRIVILEGED_CODE
void enableCpuCache() {
    unsigned long long cr0 = 0;
    asm volatile (
        "mov %%cr0, %0\n\t"          // Move CR0 into our local variable
        "and $~(0x40000000ULL), %0\n\t" // Clear CD bit (bit 30)
        "and $~(0x20000000ULL), %0\n\t" // Clear NW bit (bit 29)
        "mov %0, %%cr0\n\t"          // Move our local variable back into CR0
        : "=r"(cr0)                  // Output to local variable
        : "0"(cr0)                   // Input from the same local variable
        : "memory"
    );
}

__PRIVILEGED_CODE
void flushCpuCache() {
    asm volatile("wbinvd" : : : "memory");
}

#include <kprint.h>
__PRIVILEGED_CODE
void setMtrrWriteCombining(uint64_t base, uint64_t size) {
    // First, ensure that the base and size are aligned to 4 KiB boundaries,
    // as MTRR typically requires this. This example assumes that the size is
    // already a power of 2, as required by the MTRR specifications.
    if (!isPowerOf2(size)) {
        // Handle the error
        return;
    }

    uint64_t mtrrCap = readMsr(IA32_MTRRCAP);

    // Check if write-combining (WC) type is supported
    if (!(mtrrCap & (1ULL << 12))) {
        // Write-combining is not supported; handle this error.
        kprint("[-] Error writing MTRR: Write-combining mode is NOT supported!\n");
        return;
    }

    // Find the number of MTRR variable pairs supported by the CPU
    uint32_t vcnt = (mtrrCap & 0xFF);

    // Disable interrupts before modifying MTRRs
    disableInterrupts();

    // Disable MTRRs
    uint64_t defType = readMsr(IA32_MTRR_DEF_TYPE);
    writeMsr(IA32_MTRR_DEF_TYPE, defType & ~(1ULL << 11));

    // Invalidate caches
    flushCpuCache();

    // Set the variable MTRRs to write-combining for the framebuffer range
    for (uint32_t i = 0; i < vcnt; ++i) {
        uint64_t physmask = readMsr(IA32_MTRR_PHYSMASK(i));

        // Check if this MTRR is already used
        if (physmask & (1ULL << 11)) {
            kprint("[WARN] MTRR already in use!\n");
            continue; // This MTRR is already in use; skip it.
        }

        // Set the MTRR to Write-Combining
        writeMsr(IA32_MTRR_PHYSBASE(i), base | MTRR_WC);
        // The mask should have the valid bit set, and all other bits
        // determine the range of the region
        writeMsr(IA32_MTRR_PHYSMASK(i), ((~size + 1) & 0xFFFFFFFFFF000ULL) | (1ULL << 11));
        break; // We've set our range, so we can exit the loop
    }

    // Re-enable MTRRs
    writeMsr(IA32_MTRR_DEF_TYPE, defType | (1ULL << 11));

    // Re-enable interrupts if you previously disabled them
    enableInterrupts();
}
