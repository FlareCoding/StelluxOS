#include "trap_frame.h"
#include "defs/vectors.h"
#include "common/logging.h"
#include "sched/task_exec_core.h"
#include "percpu/percpu.h"

namespace sched {
__PRIVILEGED_CODE void on_yield(x86::trap_frame* tf);
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

    uint64_t cr2 = 0;
    if (tf->vector == x86::EXC_PAGE_FAULT) {
        cr2 = x86::read_cr2();
    }

    log::fatal(
        "x86_64 trap: vec=%lu err=0x%lx from_user=%u rip=%p cs=0x%lx rflags=0x%lx rsp=%p ss=0x%lx cr2=%p",
        tf->vector,
        tf->error_code,
        x86::from_user(tf) ? 1 : 0,
        reinterpret_cast<void*>(tf->rip),
        tf->cs,
        tf->rflags,
        reinterpret_cast<void*>(tf->rsp),
        tf->ss,
        reinterpret_cast<void*>(cr2)
    );
    
    // Clear interrupt context flag before returning
    task_core->flags &= ~sched::TASK_FLAG_IN_IRQ;
}
