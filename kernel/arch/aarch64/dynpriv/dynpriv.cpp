#include "dynpriv/dynpriv.h"
#include "syscall/syscall.h"
#include "sched/task_exec_core.h"
#include "percpu/percpu.h"
#include "defs/exception.h"
#include "common/types.h"
#include "common/logging.h"

namespace dynpriv {

void elevate() {
    asm volatile(
        "mov x8, %0\n\t"
        "svc #0"
        :
        : "r"(static_cast<uint64_t>(syscall::SYS_ELEVATE))
        : "x8", "memory"
    );
}

void lower() {
    sched::task_exec_core* task = this_cpu(current_task_exec);

    if (!(task->flags & sched::TASK_FLAG_ELEVATED)) {
        log::warn("dynpriv: task not elevated, cannot lower");
        return;
    }

    /*
     * Critical section: We must mask interrupts before clearing the
     * elevated flag and per-CPU elevation state to prevent a race where
     * an interrupt handler sees inconsistent state.
     *
     * ERET restores PSTATE from SPSR_EL1. Setting SPSR to EL0t (0x00)
     * means DAIF bits are all 0, so interrupts are unmasked after ERET.
     */
    bool* pcpu_flag = percpu::this_cpu_ptr(percpu_is_elevated);
    asm volatile(
        "msr daifset, #0xf\n\t"       /* Mask all interrupts (I, F, A, D) */
        "strb wzr, [%[pcpu]]\n\t"     /* Clear per-CPU elevation flag */
        "adr x16, 1f\n\t"             /* Load return address */
        "msr elr_el1, x16\n\t"        /* Set ELR_EL1 */
        "mov x16, %[el0t]\n\t"        /* EL0t mode (unmasks interrupts on eret) */
        "msr spsr_el1, x16\n\t"       /* Set SPSR_EL1 */
        "ldr w17, [%[flags]]\n\t"     /* Load flags */
        "bic w17, w17, #1\n\t"        /* Clear TASK_FLAG_ELEVATED (bit 0) */
        "str w17, [%[flags]]\n\t"     /* Store flags */
        "eret\n\t"                    /* Execute ERET */
        "1:"                          /* Return point in EL0 */
        :
        : [flags] "r" (&task->flags),
          [el0t] "i" (aarch64::SPSR_EL0T),
          [pcpu] "r" (pcpu_flag)
        : "x16", "x17", "memory"
    );
}

bool is_elevated() {
    return this_cpu(percpu_is_elevated);
}

} // namespace dynpriv
