#include <sched/sched.h>

namespace sched {
__PRIVILEGED_DATA
task_control_block g_idle_tasks[MAX_SYSTEM_CPUS];

__PRIVILEGED_CODE
task_control_block* get_idle_task(uint64_t cpu) {
    if (cpu > MAX_SYSTEM_CPUS - 1) {
        return nullptr;
    }

    return &g_idle_tasks[cpu];
}
} // namespace sched
