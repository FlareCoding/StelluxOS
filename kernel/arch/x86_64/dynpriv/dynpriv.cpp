#include "dynpriv/dynpriv.h"
#include "syscall/syscall.h"
#include "sched/task_exec_core.h"
#include "percpu/percpu.h"
#include "common/logging.h"

namespace dynpriv {

void elevate() {
    asm volatile(
        "syscall"
        :
        : "a"(syscall::SYS_ELEVATE)
        : "rcx", "r11", "memory"
    );
}

void lower() {
    sched::task_exec_core* task = this_cpu(current_task_exec);

    if (!(task->flags & sched::TASK_FLAG_ELEVATED)) {
        log::warn("dynpriv: task not elevated, cannot lower");
        return;
    }

    /*
     * Critical section: We must disable interrupts before clearing the
     * elevated flag and per-CPU elevation state to prevent a race where
     * an interrupt handler sees inconsistent state.
     *
     * SYSRET restores RFLAGS from R11, so interrupts are re-enabled
     * after the transition to Ring 3.
     */
    asm volatile(
        "pushfq\n\t"                  /* Push RFLAGS onto stack */
        "pop %%r11\n\t"               /* Pop into R11 (required by SYSRET) */
        "cli\n\t"                     /* Disable interrupts */
        "movb $0, %[pcpu_elev]\n\t"   /* Clear per-CPU elevation flag */
        "lea 1f(%%rip), %%rcx\n\t"    /* Load return address into RCX */
        "andl %[mask], %[flags]\n\t"  /* Clear TASK_FLAG_ELEVATED */
        "sysretq\n\t"                 /* Execute SYSRET (restores IF from R11) */
        "1:"                          /* Return point in Ring 3 */
        : [flags] "+m" (task->flags),
          [pcpu_elev] "=m" (this_cpu(percpu_is_elevated))
        : [mask] "i" (static_cast<int32_t>(~sched::TASK_FLAG_ELEVATED))
        : "rcx", "r11", "cc"
    );
}

bool is_elevated() {
    return this_cpu(percpu_is_elevated);
}

} // namespace dynpriv
