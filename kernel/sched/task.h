#ifndef STELLUX_SCHED_TASK_H
#define STELLUX_SCHED_TASK_H

#include "sched/task_exec_core.h"
#include "common/list.h"
#include "common/hashmap.h"
#include "rc/ref_counted.h"
#include "rc/reaper.h"
#include "sync/spinlock.h"
#include "resource/handle_table.h"

namespace resource::proc_provider { struct proc_resource; }
namespace fs { class node; }

namespace sched {

constexpr size_t TASK_NAME_MAX = 256;

constexpr uint32_t TASK_STATE_CREATED = 0; // exists but not on any queue
constexpr uint32_t TASK_STATE_READY   = 1; // on a runqueue
constexpr uint32_t TASK_STATE_RUNNING = 2; // executing on a CPU
constexpr uint32_t TASK_STATE_BLOCKED = 3; // on a wait queue
constexpr uint32_t TASK_STATE_DEAD    = 4; // terminated

constexpr int32_t TASK_KILL_STATUS    = 9; // wait-status for forcibly killed tasks

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

struct thread_group;

struct task {
    // Execution core
    task_exec_core exec;

    // Thread group (non-null for userland tasks, null for kernel tasks)
    thread_group*  group;
    list::node     group_link; // link in thread_group::threads (non-leaders only)

    // Identity
    uint32_t        tid;
    char            name[TASK_NAME_MAX];
    fs::node*       cwd;
    hashmap::node   task_registry_link;

    // Lifecycle
    int32_t        exit_code;
    uint32_t       state;
    uint32_t       cleanup_stage;
    uint32_t       kill_pending;

    // Stacks
    uintptr_t      task_stack_base;
    uintptr_t      sys_stack_base;

    // Scheduler state
    list::node              sched_link;
    list::node              wait_link;
    list::node              timer_link;
    uint64_t                timer_deadline;
    task_tlb_sync_ticket    tlb_sync_ticket;
    rc::reaper::dead_node   reaper_node;

    // Resources
    resource::handle_table  handles;
    resource::proc_provider::proc_resource* proc_res;
};

// Assembly accesses task_exec_core fields via offsets from the task pointer.
// exec must be at offset 0 so &task == &task.exec.
static_assert(__builtin_offsetof(task, exec) == 0,
    "task.exec must be at offset 0 for assembly compatibility");

/**
 * Groups all tasks sharing an address space. Every userland task belongs to
 * exactly one thread_group. The leader creates the group at process
 * creation, threads are added when spawned. The group outlives any
 * individual task via ref-counting and is freed when the last task
 * (leader or thread) releases its reference.
 */
struct thread_group : rc::ref_counted<thread_group> {
    sync::spinlock lock;
    task*          leader;
    list::head<task, &task::group_link> threads; // non-leader threads only
    uint32_t       thread_count; // number of live non-leader threads

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static void ref_destroy(thread_group* self);
};

} // namespace sched

#endif // STELLUX_SCHED_TASK_H
