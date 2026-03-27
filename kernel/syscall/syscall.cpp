#include "syscall/syscall.h"
#include "syscall/syscall_table.h"
#include "sched/task_exec_core.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "dynpriv/dynpriv.h"
#include "percpu/percpu.h"

constexpr uint32_t ELEVATION_CONTEXT_MASK =
    sched::TASK_FLAG_ELEVATED | sched::TASK_FLAG_IN_SYSCALL | sched::TASK_FLAG_IN_IRQ;

__PRIVILEGED_CODE static inline void restore_post_syscall_elevation_state() {
    // Return-boundary restoration: select runtime elevation based on the
    // currently selected task's privilege-mode bit, plus any active
    // elevated context (in-syscall or in-IRQ).
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
    // Mark as elevated so RUN_ELEVATED inside handlers skips nested SYSCALL.
    // The SYSCALL entry already switched to Ring 0 and the system stack.
    // IN_SYSCALL ensures percpu_is_elevated is correctly restored if the
    // handler sleeps and a context switch occurs mid-syscall.
    this_cpu(current_task_exec)->flags |= sched::TASK_FLAG_IN_SYSCALL;
    this_cpu(percpu_is_elevated) = true;

    int64_t result;

    if (syscall_num < syscall::MAX_SYSCALL_NUM && syscall::g_syscall_table[syscall_num]) {
        result = syscall::g_syscall_table[syscall_num](arg1, arg2, arg3, arg4, arg5, arg6);
    } else {
        result = syscall::ENOSYS;
    }

    sched::task* self = sched::current();
    if (self && __atomic_load_n(&self->kill_pending, __ATOMIC_ACQUIRE)
        && !(self->exec.flags & sched::TASK_FLAG_KERNEL)) {
        sched::exit(0x9);
    }

    // Return-boundary restore: dynamic runtime elevation follows the selected
    // task mode once syscall handling and switch teardown are complete.
    this_cpu(current_task_exec)->flags &= ~sched::TASK_FLAG_IN_SYSCALL;
    restore_post_syscall_elevation_state();

    return result;
}
