#ifndef STELLUX_SCHED_TASK_H
#define STELLUX_SCHED_TASK_H

#include "sched/task_exec_core.h"
#include "common/list.h"
#include "rc/reaper.h"

namespace sched {

constexpr uint32_t TASK_STATE_CREATED = 0; // exists but not on any queue
constexpr uint32_t TASK_STATE_READY   = 1; // on a runqueue
constexpr uint32_t TASK_STATE_RUNNING = 2; // executing on a CPU
constexpr uint32_t TASK_STATE_BLOCKED = 3; // on a wait queue
constexpr uint32_t TASK_STATE_DEAD    = 4; // terminated

constexpr uint32_t TASK_CLEANUP_STAGE_ACTIVE                = 0;
constexpr uint32_t TASK_CLEANUP_STAGE_EXIT_REQUESTED        = 1;
constexpr uint32_t TASK_CLEANUP_STAGE_SCHEDULER_DETACHED    = 2;
constexpr uint32_t TASK_CLEANUP_STAGE_WAITING_FOR_TLB_SYNC  = 3;
constexpr uint32_t TASK_CLEANUP_STAGE_READY_TO_RECLAIM      = 4;

/**
 * Per-task TLB sync ticket used by reaper before reclaiming task stacks.
 *
 * The ticket snapshots each CPU's reclaim epoch and requires every CPU to
 * advance past that snapshot before stack unmap/free can proceed.
 */
struct task_tlb_sync_ticket {
    uint64_t cpu_epoch_snapshot[MAX_CPUS];
    uint32_t armed;
};

struct task {
    task_exec_core exec;
    uint32_t       tid;
    int32_t        exit_code;
    uint32_t       state;
    uint32_t       cleanup_stage;
    uintptr_t      task_stack_base;
    uintptr_t      sys_stack_base;
    list::node     sched_link;
    list::node     wait_link;
    list::node     timer_link;
    uint64_t       timer_deadline;
    const char*    name;
    task_tlb_sync_ticket tlb_sync_ticket;
    rc::reaper::dead_node reaper_node;
};

// Assembly accesses task_exec_core fields via offsets from the task pointer.
// exec must be at offset 0 so &task == &task.exec.
static_assert(__builtin_offsetof(task, exec) == 0,
    "task.exec must be at offset 0 for assembly compatibility");

} // namespace sched

#endif // STELLUX_SCHED_TASK_H
