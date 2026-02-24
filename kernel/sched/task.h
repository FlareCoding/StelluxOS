#ifndef STELLUX_SCHED_TASK_H
#define STELLUX_SCHED_TASK_H

#include "sched/task_exec_core.h"
#include "common/list.h"

namespace sched {

constexpr uint32_t TASK_STATE_READY   = 0;
constexpr uint32_t TASK_STATE_RUNNING = 1;
constexpr uint32_t TASK_STATE_DEAD    = 2;

struct task {
    task_exec_core exec;
    uint32_t       tid;
    int32_t        exit_code;
    uint32_t       state;
    list::node     sched_link;
    const char*    name;
};

// Assembly accesses task_exec_core fields via offsets from the task pointer.
// exec must be at offset 0 so &task == &task.exec.
static_assert(__builtin_offsetof(task, exec) == 0,
    "task.exec must be at offset 0 for assembly compatibility");

} // namespace sched

#endif // STELLUX_SCHED_TASK_H
