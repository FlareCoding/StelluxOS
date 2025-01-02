#ifdef ARCH_X86_64
#ifndef CPU_CONTROL_H
#define CPU_CONTROL_H
#include <types.h>

#define CR0_PE (1UL << 0)  // Protected Mode Enable
#define CR0_MP (1UL << 1)  // Monitor co-processor
#define CR0_EM (1UL << 2)  // x87 FPU Emulation
#define CR0_TS (1UL << 3)  // Task switched
#define CR0_ET (1UL << 4)  // Extension type
#define CR0_NE (1UL << 5)  // Numeric error
#define CR0_WP (1UL << 16) // Write protect
#define CR0_AM (1UL << 18) // Alignment mask
#define CR0_NW (1UL << 29) // Not-write through
#define CR0_CD (1UL << 30) // Cache disable
#define CR0_PG (1UL << 31) // Paging

#define CR4_VME        (1UL << 0)   // Virtual 8086 Mode Extensions
#define CR4_PVI        (1UL << 1)   // Protected-mode Virtual Interrupts
#define CR4_TSD        (1UL << 2)   // Time Stamp Disable
#define CR4_DE         (1UL << 3)   // Debugging Extensions
#define CR4_PSE        (1UL << 4)   // Page Size Extension
#define CR4_PAE        (1UL << 5)   // Physical Address Extension
#define CR4_MCE        (1UL << 6)   // Machine Check Exception
#define CR4_PGE        (1UL << 7)   // Page Global Enabled
#define CR4_PCE        (1UL << 8)   // Performance-Monitoring Counter enable
#define CR4_OSFXSR     (1UL << 9)   // Operating system support for FXSAVE and FXRSTOR instructions
#define CR4_OSXMMEXCPT (1UL << 10)  // Operating System Support for Unmasked SIMD Floating-Point Exceptions
#define CR4_UMIP       (1UL << 11)  // User-Mode Instruction Prevention
#define CR4_VMXE       (1UL << 13)  // Virtual Machine Extensions Enable
#define CR4_SMXE       (1UL << 14)  // Safer Mode Extensions Enable
#define CR4_FSGSBASE   (1UL << 16)  // Enables the instructions RDFSBASE, RDGSBASE, WRFSBASE, and WRGSBASE
#define CR4_PCIDE      (1UL << 17)  // PCID Enable
#define CR4_OSXSAVE    (1UL << 18)  // XSAVE and Processor Extended States Enable
#define CR4_SMEP       (1UL << 20)  // Supervisor Mode Execution Protection Enable
#define CR4_SMAP       (1UL << 21)  // Supervisor Mode Access Prevention Enable
#define CR4_PKE        (1UL << 22)  // Protection Key Enable
#define CR4_CET        (1UL << 23)  // Control-flow Enforcement Technology
#define CR4_PKS        (1UL << 24)  // Enable Protection Keys for Supervisor-Mode Pages

namespace arch::x86 {
/**
 * @brief Disables the CPU cache and retrieves the previous CR0 register value.
 * @param old_cr0 Pointer to store the previous value of the CR0 register.
 * 
 * Modifies the CR0 register to disable the CPU cache and stores the old value for later restoration.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void cpu_cache_disable(uint64_t* old_cr0);

/**
 * @brief Sets the CR0 register to a specified value.
 * @param cr0 The value to set in the CR0 register.
 * 
 * Updates the CR0 register to the provided value, modifying system behavior as required.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void cpu_set_cr0(uint64_t cr0);

/**
 * @brief Flushes the CPU cache.
 * 
 * Ensures all modified cache lines are written back to memory and invalidates the cache,
 * forcing subsequent memory operations to fetch from main memory.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void cpu_cache_flush();

/**
 * @brief Clears the Page Global Enable (PGE) bit in the CR4 register.
 * 
 * Disables the Page Global feature, invalidating global page entries in the TLB.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void cpu_pge_clear();

/**
 * @brief Enables the Page Global Enable (PGE) bit in the CR4 register.
 * 
 * Activates the Page Global feature, allowing global page entries to remain in the TLB
 * across context switches.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void cpu_pge_enable();
} // namespace arch::x86

#endif // CPU_CONTROL_H
#endif // ARCH_X86_64
