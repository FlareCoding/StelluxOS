#include "arch/arch_init.h"
#include "percpu/percpu.h"
#include "trap/trap.h"
#include "cpu/features.h"
#include "sched/task_exec_core.h"
#include "syscall/syscall.h"

namespace arch {

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t early_init() {
    // Initialize per-CPU for BSP (sets TPIDR_EL1)
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

    // Initialize syscall dispatch for dynamic privilege
    if (syscall::init_arch_syscalls() != syscall::OK) {
        return ERR_SYSCALL_INIT;
    }

    return OK;
}

} // namespace arch
