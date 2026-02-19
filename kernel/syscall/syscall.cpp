#include "syscall/syscall.h"
#include "sched/task_exec_core.h"
#include "percpu/percpu.h"
#include "common/utils/logging.h"

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int64_t handle_sys_elevate() {
    sched::task_exec_core* task = this_cpu(current_task);

    if (!(task->flags & sched::TASK_FLAG_CAN_ELEVATE)) {
        log::fatal("syscall: task not authorized to elevate");
    }

    if (task->flags & sched::TASK_FLAG_ELEVATED) {
        log::warn("syscall: task already elevated");
        return 0;
    }

    task->flags |= sched::TASK_FLAG_ELEVATED;
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

    switch (syscall_num) {
        case syscall::SYS_ELEVATE:
            return handle_sys_elevate();
        default:
            log::warn("syscall: unknown syscall %lu", syscall_num);
            return -1;
    }
}
