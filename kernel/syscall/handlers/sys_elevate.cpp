#include "syscall/handlers/sys_elevate.h"
#include "sched/task_exec_core.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "dynpriv/dynpriv.h"
#include "percpu/percpu.h"
#include "common/logging.h"

DEFINE_SYSCALL0(elevate) {
    sched::task_exec_core* task = this_cpu(current_task_exec);
    if (!(task->flags & sched::TASK_FLAG_CAN_ELEVATE)) {
        sched::task* caller = sched::current();
        log::warn("syscall: task not authorized to elevate tid=%u name=%s", caller->tid, caller->name);
        return syscall::EPERM;
    }

    if (task->flags & sched::TASK_FLAG_ELEVATED) {
        log::warn("syscall: task already elevated");
        return 0;
    }

    task->flags |= sched::TASK_FLAG_ELEVATED;
    this_cpu(percpu_is_elevated) = true;
    return 0;
}
