#include "trap_frame.h"
#include "defs/vectors.h"
#include "irq/irq.h"
#include "timer/timer.h"
#include "debug/panic.h"
#include "sched/task_exec_core.h"
#include "percpu/percpu.h"
#include "dynpriv/dynpriv.h"

namespace sched {
__PRIVILEGED_CODE void on_yield(x86::trap_frame* tf);
__PRIVILEGED_CODE void on_tick(x86::trap_frame* tf);
} // namespace sched

__PRIVILEGED_CODE static inline void restore_post_trap_elevation_state() {
    // Return-boundary restoration: select runtime elevation based on the
    // currently selected task's privilege-mode bit.
    this_cpu(percpu_is_elevated) =
        (this_cpu(current_task_exec)->flags & sched::TASK_FLAG_ELEVATED) != 0;
}

/**
 * @brief x86_64 trap handler called from assembly.
 * @note Privilege: **required**
 */
extern "C" __PRIVILEGED_CODE void stlx_x86_64_trap_handler(x86::trap_frame* tf) {
    this_cpu(percpu_is_elevated) = true;

    sched::task_exec_core* irq_task_core = this_cpu(current_task_exec);
    
    // Mark as in interrupt context
    irq_task_core->flags |= sched::TASK_FLAG_IN_IRQ;

    if (tf->vector == x86::VEC_SCHED_YIELD) {
        sched::on_yield(tf);
        // Clear the IRQ flag on the originally interrupted task, not the post-switch task.
        irq_task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
        restore_post_trap_elevation_state();
        return;
    }

    if (tf->vector == x86::VEC_TIMER) {
        irq::eoi(0);
        bool tick = timer::on_interrupt();
        if (tick) {
            sched::on_tick(tf);
        }
        // Clear IRQ state on the interrupted task to avoid stale IN_IRQ ownership.
        irq_task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
        restore_post_trap_elevation_state();
        return;
    }

    panic::on_trap(tf);
}
