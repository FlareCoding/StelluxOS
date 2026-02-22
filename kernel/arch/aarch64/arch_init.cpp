#include "arch/arch_init.h"
#include "percpu/percpu.h"
#include "trap/trap.h"
#include "cpu/features.h"
#include "sched/task_exec_core.h"
#include "syscall/syscall.h"
#include "common/logging.h"

namespace arch {

/**
 * Switch from EL1h (SPSEL=1) to EL1t (SPSEL=0) so all schedulable tasks
 * use SP_EL0 as their task stack and SP_EL1 as the system stack.
 *
 * After this, exceptions from the boot task route through
 * STLX_A64_EL1_SP0_ENTRY which correctly saves/restores SP_EL0.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void switch_to_el1t() {
    extern char stack_top[];
    asm volatile(
        "mov x0, sp\n\t"
        "msr sp_el0, x0\n\t"
        "adrp x1, stack_top\n\t"
        "add x1, x1, :lo12:stack_top\n\t"
        "mov sp, x1\n\t"
        "msr spsel, #0\n\t"
        "isb"
        ::: "x0", "x1", "memory"
    );
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t early_init() {
    // Must be first so that per-CPU variables can be used by subsequent init operations
    if (percpu::init_bsp() != percpu::OK) {
        return ERR_PERCPU_INIT;
    }

    // Initialize VBAR_EL1 (trap vectors)
    if (trap::init() != trap::OK) {
        return ERR_TRAP_INIT;
    }

    // Detect CPU features via ID registers
    if (cpu::init() != cpu::OK) {
        return ERR_CPU_INIT;
    }

    // Initialize boot task for scheduler/dynpriv tracking
    if (sched::init_boot_task() != 0) {
        return ERR_SCHED_INIT;
    }

    // Switch to EL1t so exceptions use STLX_A64_EL1_SP0_ENTRY path.
    // Must be after init_boot_task (boot task struct set up) and before
    // syscall init (which may trigger SVC via dynpriv).
    switch_to_el1t();

    // Initialize syscall dispatch for dynamic privilege
    if (syscall::init_arch_syscalls() != syscall::OK) {
        return ERR_SYSCALL_INIT;
    }

    return OK;
}

} // namespace arch
