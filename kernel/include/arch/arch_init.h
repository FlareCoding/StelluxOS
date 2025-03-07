#ifndef ARCH_INIT_H
#define ARCH_INIT_H
#include <types.h>

namespace arch {
/**
 * @brief Initializes architecture-specific components during system startup.
 * 
 * This function performs the following steps to set up the architecture-specific
 * environment:
 * 
 * 1. **Setup Kernel Stack:**
 * Initializes the kernel stack for the Bootstrap Processor (BSP) by calculating
 * the top address of the default BSP system stack.
 * 
 * 2. **Initialize Global Descriptor Table (GDT):**
 * Configures the GDT with support for user space by invoking `x86::init_gdt`
 * with the BSP CPU ID and the calculated stack top.
 * 
 * 3. **Initialize Interrupt Descriptor Table (IDT) and Enable Interrupts:**
 * Sets up the IDT using `x86::init_idt` and enables CPU interrupts by
 * calling `enable_interrupts()`.
 * 
 * 4. **Setup the kernel pat:**
 * Sets the the page attribute table to contain a write-combining entry.
 * 
 * 5. **Enable FSGSBASE Instructions and Initialize Per-CPU Area:**
 * Enables the FSGSBASE instructions via `x86::enable_fsgsbase()` and initializes
 * the per-CPU area for the bootstrapping processor with `init_bsp_per_cpu_area()`.
 * 
 * 6. **Setup BSP's Idle Task:**
 * Retrieves the idle task control block for the BSP CPU using `sched::get_idle_task`,
 * zeroes out its memory, and assigns it to the current CPU's `current_task`.
 * 
 * 7. **Configure Current Task Properties:**
 * Sets up the current task's system stack, CPU identifier, privilege level,
 * state, and process ID (`pid`) to their initial values.
 * 
 * 8. **Enable System Call Interface:**
 * Activates the system call interface by calling `enable_syscall_interface()`,
 * allowing user-space applications to request kernel services.
 * 
 * 9. **Setup and Enable Dynamic Privilege Mechanism:**
 * Configures the dynamic privilege mechanism by setting the current ASID
 * for elevation checks using `dynpriv::use_current_asid()`.
 */
__PRIVILEGED_CODE void arch_init();

/**
 * @brief Initializes late-stage architecture specific components.
 * 
 * This function must be called AFTER virtual memory manager has been initialized.
 */
__PRIVILEGED_CODE void arch_late_stage_init();

/**
 * @brief Sets up and installs an interrupt handler for COM1 serial input.
 * 
 * Performs appropriate architecture-specific IRQ routing (e.g. IOAPIC on x86)
 * and installs the interrupt handler for COM1 serial input interrupts.
 */
__PRIVILEGED_CODE void setup_com1_irq();
} // namespace arch

#endif
