#ifndef FPU_H
#define FPU_H
#include <types.h>
#include <arch/percpu.h>
#include <interrupts/irq.h>

#define __FPU_ALIGNMENT 16

// Forward declaration
struct process_core;

/**
 * @brief Per-CPU variable for tracking which process owns the FPU.
 */
DECLARE_PER_CPU(process_core*, fpu_owner);

/**
 * @brief Per-CPU variable for tracking if FPU was used in IRQ context.
 */
DECLARE_PER_CPU(bool, fpu_used_in_irq);

namespace fpu {

/**
 * @brief FPU (Floating Point Unit) state for SSE operations.
 * 
 * This structure contains the complete FPU/SSE state that needs to be
 * saved and restored during context switches. It's aligned to 16 bytes
 * as required by the FXSAVE/FXRSTOR instructions.
 */
struct fpu_state {
    // FPU state tracking flags come first to avoid affecting alignment
    uint8_t has_used_fpu     : 1;  // Has this process ever used FPU
    uint8_t needs_fpu_save   : 1;  // Does FPU state need to be saved
    uint8_t fpu_initialized  : 1;  // Has FPU state been initialized
    uint8_t reserved         : 5;  // Reserved for future use
    
    // Padding to ensure fxsave_area starts at 16-byte boundary
    uint8_t padding[15];
    
    // The FXSAVE area must be 16-byte aligned and exactly 512 bytes
    uint8_t fxsave_area[512] __attribute__((aligned(__FPU_ALIGNMENT)));
};

/**
 * @brief Initializes the FPU state for a process core.
 * @param core Pointer to the process core to initialize FPU state for.
 * 
 * Sets up the initial FPU state with default values and marks it as initialized.
 * This should be called for every new process core.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void init_fpu_state(process_core* core);

/**
 * @brief Saves the current FPU state to the specified process core.
 * @param core Pointer to the process core to save FPU state to.
 * 
 * Uses FXSAVE instruction to save the complete FPU/SSE state.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void save_fpu_state(process_core* core);

/**
 * @brief Restores FPU state from the specified process core.
 * @param core Pointer to the process core to restore FPU state from.
 * 
 * Uses FXRSTOR instruction to restore the complete FPU/SSE state.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void restore_fpu_state(process_core* core);

/**
 * @brief Clears the FPU state for security purposes.
 * 
 * Zeros out the FPU registers to prevent information leakage between processes.
 * This should be called when switching to a process that has never used FPU.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void clear_fpu_state();

/**
 * @brief Enables FPU by clearing the TS (Task Switched) bit in CR0.
 * 
 * Uses the CLTS instruction to quickly enable FPU operations.
 * After this call, FPU instructions will execute normally.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void enable();

/**
 * @brief Disables FPU by setting the TS (Task Switched) bit in CR0.
 * 
 * After this call, FPU instructions will trigger #NM exceptions.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void disable();

/**
 * @brief Checks if FPU is currently enabled.
 * 
 * @return true if FPU is enabled (TS bit clear), false if disabled (TS bit set)
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool is_enabled();

#ifdef ARCH_X86_64

DEFINE_INT_HANDLER(exc_nm_fpu_handler);

#endif // ARCH_X86_64
} // namespace fpu

#endif // FPU_H
