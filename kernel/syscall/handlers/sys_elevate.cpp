#include "syscall/handlers/sys_elevate.h"
#include "sched/task_exec_core.h"
#include "dynpriv/dynpriv.h"
#include "percpu/percpu.h"
#include "common/logging.h"

DEFINE_SYSCALL0(elevate) {
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
