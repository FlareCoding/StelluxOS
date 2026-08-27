#include "syscall/syscall.h"
#include "syscall/syscall_table.h"
#include "arch/arch_signal.h"
#include "sched/task_exec_core.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "signals/signal.h"
#include "dynpriv/dynpriv.h"
#include "percpu/percpu.h"
#include "common/logging.h"

constexpr uint32_t ELEVATION_CONTEXT_MASK = sched::TASK_FLAG_ELEVATED | sched::TASK_FLAG_IN_SYSCALL;

__PRIVILEGED_CODE static inline void restore_post_syscall_elevation_state() {
    // Select runtime elevation from the selected task's privilege-mode
    // bit plus any active elevated context (in-syscall or in-IRQ).
    this_cpu(percpu_is_elevated) =
        (this_cpu(current_task_exec)->flags & ELEVATION_CONTEXT_MASK) != 0;
}

/**
 * @note Privilege: **required**
 */
extern "C" __PRIVILEGED_CODE int64_t stlx_syscall_handler(
    uint64_t syscall_num,
    uint64_t arg1,
    uint64_t arg2,
    uint64_t arg3,
    uint64_t arg4,
    uint64_t arg5,
    uint64_t arg6
) {
    // Entry already runs at Ring 0, so RUN_ELEVATED skips nested SYSCALLs.
    // IN_SYSCALL keeps percpu_is_elevated correct across a mid-syscall switch.
    this_cpu(current_task_exec)->flags |= sched::TASK_FLAG_IN_SYSCALL;
    this_cpu(percpu_is_elevated) = true;

    int64_t result;

    if (syscall_num < syscall::MAX_SYSCALL_NUM && syscall::g_syscall_table[syscall_num]) {
        result = syscall::g_syscall_table[syscall_num](arg1, arg2, arg3, arg4, arg5, arg6);
    } else {
        sched::task* caller = sched::current();
        log::warn("syscall: unimplemented nr=%lu from tid=%d",
                  syscall_num, caller ? caller->tid : -1);
        result = syscall::ENOSYS;
    }

    sched::task* self = sched::current();
    if (self && !(self->exec.flags & sched::TASK_FLAG_KERNEL)) {
        uint32_t fsig = signals::fatal_pending(self);
        if (fsig) {
            signals::die_from_signal(fsig);
        }

        // Delivers one pending handled signal and resolves interrupted-wait
        // restarts, the internal ERESTARTSYS marker never reaches userspace
        result = arch::deliver_pending_signal(self, result, syscall_num);
    }

    // Return-boundary restore: dynamic runtime elevation follows the selected
    // task mode once syscall handling and switch teardown are complete.
    this_cpu(current_task_exec)->flags &= ~sched::TASK_FLAG_IN_SYSCALL;
    restore_post_syscall_elevation_state();

    return result;
}
