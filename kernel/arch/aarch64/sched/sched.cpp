#include "sched/task_exec_core.h"
#include "percpu/percpu.h"
#include "common/types.h"
#include "logging.h"

extern "C" char stack_top[];

DEFINE_PER_CPU(sched::task_exec_core*, current_task);

namespace sched {

static task_exec_core g_boot_task = {
    .flags = TASK_FLAG_ELEVATED | TASK_FLAG_KERNEL | TASK_FLAG_CAN_ELEVATE 
           | TASK_FLAG_IDLE | TASK_FLAG_RUNNING,
    .cpu = 0,
    .task_stack_top = 0,
    .system_stack_top = 0,
    .cpu_ctx = {},
};

__PRIVILEGED_CODE int32_t init_boot_task() {
    g_boot_task.task_stack_top = reinterpret_cast<uintptr_t>(stack_top);
    g_boot_task.system_stack_top = reinterpret_cast<uintptr_t>(stack_top) - 0x40;

    this_cpu(current_task) = &g_boot_task;
    log::info("sched: boot task initialized, flags=0x%x", g_boot_task.flags);
    return 0;
}

} // namespace sched
