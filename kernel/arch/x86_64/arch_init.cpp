#include "arch/arch_init.h"
#include "percpu/percpu.h"
#include "gdt/gdt.h"
#include "trap/trap.h"
#include "cpu/features.h"
#include "sched/task_exec_core.h"
#include "syscall/syscall.h"

namespace arch {

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t early_init() {
    // Initialize per-CPU for BSP (sets GS base via WRMSR)
    // Must be first so that per-CPU variables can be used by subsequent init operations
    if (percpu::init_bsp() != percpu::OK) {
        return ERR_PERCPU_INIT;
    }

    // Initialize GDT/TSS
    if (x86::gdt::init_bsp() != x86::gdt::OK) {
        return ERR_GDT_INIT;
    }
    x86::gdt::load();

    // Initialize IDT - must be before cpu::init() so #GP from bad CR4/MSR writes are catchable
    if (trap::init() != trap::OK) {
        return ERR_TRAP_INIT;
    }

    // Detect CPU features and enable FSGSBASE + PAT
    if (cpu::init() != cpu::OK) {
        return ERR_CPU_INIT;
    }

    // Initialize boot task for scheduler/dynpriv tracking
    if (sched::init_boot_task() != 0) {
        return ERR_SCHED_INIT;
    }

    // Initialize syscall/sysret MSRs for dynamic privilege
    if (syscall::init_arch_syscalls() != syscall::OK) {
        return ERR_SYSCALL_INIT;
    }

    return OK;
}

} // namespace arch
