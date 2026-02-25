#ifndef STELLUX_SCHED_TASK_H
#define STELLUX_SCHED_TASK_H

#include "sched/task_exec_core.h"
#include "common/list.h"

namespace sched {

constexpr uint32_t TASK_STATE_CREATED = 0; // exists but not on any queue
constexpr uint32_t TASK_STATE_READY   = 1; // on a runqueue
constexpr uint32_t TASK_STATE_RUNNING = 2; // executing on a CPU
constexpr uint32_t TASK_STATE_BLOCKED = 3; // on a wait queue
constexpr uint32_t TASK_STATE_DEAD    = 4; // terminated

struct task {
    task_exec_core exec;
    uint32_t       tid;
    int32_t        exit_code;
    uint32_t       state;
    list::node     sched_link;
    list::node     wait_link;
    const char*    name;
};

// Assembly accesses task_exec_core fields via offsets from the task pointer.
// exec must be at offset 0 so &task == &task.exec.
static_assert(__builtin_offsetof(task, exec) == 0,
    "task.exec must be at offset 0 for assembly compatibility");

} // namespace sched

#endif // STELLUX_SCHED_TASK_H
