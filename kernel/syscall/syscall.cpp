#include "syscall/syscall.h"
#include "sched/task_exec_core.h"
#include "dynpriv/dynpriv.h"
#include "percpu/percpu.h"
#include "common/logging.h"

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int64_t handle_sys_elevate() {
    sched::task_exec_core* task = this_cpu(current_task_exec);

    if (!(task->flags & sched::TASK_FLAG_CAN_ELEVATE)) {
        log::fatal("syscall: task not authorized to elevate");
    }

    if (task->flags & sched::TASK_FLAG_ELEVATED) {
        log::warn("syscall: task already elevated");
        return 0;
    }

    task->flags |= sched::TASK_FLAG_ELEVATED;
    this_cpu(percpu_is_elevated) = true;
    return 0;
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
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    // Mark as elevated so RUN_ELEVATED inside handlers skips nested SYSCALL.
    // The SYSCALL entry already switched to Ring 0 and the system stack.
    this_cpu(percpu_is_elevated) = true;

    int64_t result;

    switch (syscall_num) {
        case syscall::SYS_ELEVATE:
            result = handle_sys_elevate();
            break;
        default:
            log::warn("syscall: unknown syscall %lu", syscall_num);
            result = -1;
            break;
    }

    // Restore elevation state: keep elevated only if SYS_ELEVATE set the flag.
    this_cpu(percpu_is_elevated) =
        (this_cpu(current_task_exec)->flags & sched::TASK_FLAG_ELEVATED) != 0;

    return result;
}
