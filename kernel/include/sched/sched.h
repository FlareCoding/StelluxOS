#ifndef SCHED_H
#define SCHED_H
#include <process/process.h>

namespace sched {
__PRIVILEGED_CODE
task_control_block* get_idle_task(uint64_t cpu);
} // namespace sched

#endif // SCHED_H
