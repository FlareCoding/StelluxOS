#ifdef ARCH_X86_64
#ifndef FSGSBASE_H
#define FSGSBASE_H
#include <types.h>

namespace arch::x86 {
/**
 * @brief Enables the FSGSBASE instructions by setting the corresponding bit in CR4.
 * 
 * This function modifies the CR4 control register to enable the use of FSGSBASE instructions,
 * which allow direct access to the FS and GS base registers. Enabling this feature is necessary
 * for certain low-level operations that require manipulating these segment base addresses.
 */
static __force_inline__ void enable_fsgsbase() {
    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 16); // Set the FSGSBASE bit
    asm volatile("mov %0, %%cr4" :: "r"(cr4));
}

/**
 * @brief Reads the FS base register.
 * 
 * This function retrieves the current value of the FS base register using the `rdfsbase` instruction.
 * The FS base register holds the base address for the FS segment, which is used in thread-local storage
 * and other segment-based operations.
 * 
 * @return uint64_t The current value of the FS base register.
 */
static __force_inline__ uint64_t rdfsbase() {
    uint64_t base;
    asm volatile("rdfsbase %0" : "=r"(base));
    return base;
}

/**
 * @brief Writes a value to the FS base register.
 * 
 * This function sets the FS base register to the specified value using the `wrfsbase` instruction.
 * Modifying the FS base register is essential for configuring thread-local storage and other
 * operations that rely on the FS segment.
 * 
 * @param base The value to set as the new FS base register.
 */
static __force_inline__ void wrfsbase(uint64_t base) {
    asm volatile("wrfsbase %0" :: "r"(base));
}

/**
 * @brief Reads the GS base register.
 * 
 * This function retrieves the current value of the GS base register using the `rdgsbase` instruction.
 * The GS base register holds the base address for the GS segment, which is utilized in various
 * system-level and thread-specific operations.
 * 
 * @return uint64_t The current value of the GS base register.
 */
static __force_inline__ uint64_t rdgsbase() {
    uint64_t base;
    asm volatile("rdgsbase %0" : "=r"(base));
    return base;
}

/**
 * @brief Writes a value to the GS base register.
 * 
 * This function sets the GS base register to the specified value using the `wrgsbase` instruction.
 * Adjusting the GS base register is necessary for configuring thread-local storage and other
 * segment-dependent functionalities.
 * 
 * @param base The value to set as the new GS base register.
 */
static __force_inline__ void wrgsbase(uint64_t base) {
    asm volatile("wrgsbase %0" :: "r"(base));
}

/**
 * @brief Swaps the current GS base with the Kernel GS base.
 * 
 * This function executes the `swapgs` instruction, which swaps the current value of the GS base
 * register with the value stored in the Kernel GS base MSR (Model-Specific Register). This is
 * typically used when transitioning between user mode and kernel mode to maintain separate
 * GS base values for each mode.
 */
static __force_inline__ void swapgs() {
    asm volatile ("swapgs");
}
} // namespace arch::x86

#endif // FSGSBASE_H
#endif // ARCH_X86_64
