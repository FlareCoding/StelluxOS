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
        "svc %0"
        :
        : "i"(syscall::SYS_ELEVATE)
        : "memory"
    );
}

void lower() {
    sched::task_exec_core* task = this_cpu(current_task);

    if (!(task->flags & sched::TASK_FLAG_ELEVATED)) {
        log::warn("dynpriv: task not elevated, cannot lower");
        return;
    }

    /*
     * Critical section: We must mask interrupts before clearing the
     * elevated flag to prevent a race condition where an interrupt handler
     * sees the task as non-elevated while still in EL1.
     *
     * ERET restores PSTATE from SPSR_EL1. Setting SPSR to EL0t (0x00)
     * means DAIF bits are all 0, so interrupts are unmasked after ERET.
     */
    asm volatile(
        "msr daifset, #0xf\n\t"       /* Mask all interrupts (I, F, A, D) */
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
          [el0t] "i" (aarch64::SPSR_EL0T)
        : "x16", "x17", "memory"
    );
}

bool is_elevated() {
    return (this_cpu(current_task)->flags & sched::TASK_FLAG_ELEVATED) != 0;
}

} // namespace dynpriv
