#include "trap_frame.h"
#include "defs/vectors.h"
#include "irq/irq.h"
#include "debug/panic.h"
#include "sched/task_exec_core.h"
#include "percpu/percpu.h"

namespace sched {
__PRIVILEGED_CODE void on_yield(x86::trap_frame* tf);
__PRIVILEGED_CODE void on_tick(x86::trap_frame* tf);
} // namespace sched

/**
 * @brief x86_64 trap handler called from assembly.
 * @note Privilege: **required**
 */
extern "C" __PRIVILEGED_CODE void stlx_x86_64_trap_handler(x86::trap_frame* tf) {
    sched::task_exec_core* task_core = this_cpu(current_task_exec);
    
    // Mark as in interrupt context
    task_core->flags |= sched::TASK_FLAG_IN_IRQ;

    if (tf->vector == x86::VEC_SCHED_YIELD) {
        sched::on_yield(tf);
        task_core = this_cpu(current_task_exec); // may have switched tasks
        task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
        return;
    }

    if (tf->vector == x86::VEC_TIMER) {
        irq::eoi(0);
        sched::on_tick(tf);
        task_core = this_cpu(current_task_exec); // may have switched tasks
        task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
        return;
    }

    panic::on_trap(tf);
}
