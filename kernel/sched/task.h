#ifndef STELLUX_SCHED_TASK_H
#define STELLUX_SCHED_TASK_H

#include "sched/task_exec_core.h"
#include "common/list.h"
#include "common/hashmap.h"
#include "rc/ref_counted.h"
#include "rc/reaper.h"
#include "signals/signal_types.h"
#include "sync/atomic.h"
#include "sync/spinlock.h"
#include "resource/handle_table.h"

namespace resource::proc_provider { struct proc_resource; }
namespace fs { class node; }

namespace sched {

constexpr size_t TASK_NAME_MAX = 256;

/**
 * Task states and the legal transitions between them:
 *
 *   CREATED -> READY    enqueue / enqueue_on (CAS)
 *   CREATED -> DEAD     unstarted task teardown claims it (CAS)
 *   READY   -> RUNNING  pick_next_and_switch
 *   RUNNING -> READY    preemption re-enqueue
 *   RUNNING -> BLOCKED  prepare_to_block_task, by the task itself
 *   RUNNING -> DEAD     exit, by the task itself
 *   BLOCKED -> READY    wake (CAS)
 *   BLOCKED -> RUNNING  cancel_block_task, by the task itself (CAS)
 *
 * A task is BLOCKED yet still on-CPU between prepare_to_block_task and
 * its yield. A tick in that window switches it out without re-enqueueing
 * it, so it stays off the runqueues until a wake arrives.
 */
constexpr uint32_t TASK_STATE_CREATED = 0; // exists but not on any queue
constexpr uint32_t TASK_STATE_READY   = 1; // on a runqueue
constexpr uint32_t TASK_STATE_RUNNING = 2; // executing on a CPU
constexpr uint32_t TASK_STATE_BLOCKED = 3; // on a wait queue
constexpr uint32_t TASK_STATE_DEAD    = 4; // terminated

constexpr int32_t TASK_KILL_STATUS    = 9; // wait-status for forcibly killed tasks

/**
 * Reclamation ladder. exit() records EXIT_REQUESTED on the dying task, the
 * scheduler advances to SCHEDULER_DETACHED when the task is switched out
 * (unstarted teardown jumps there directly), and the reaper owns the last
 * two stages: it snapshots every CPU's TLB sync epoch, waits for each CPU
 * to move past its snapshot, then reclaims. The struct itself is freed only
 * after the last counted reference has dropped and handed it to the reaper.
 */
constexpr uint32_t TASK_CLEANUP_STAGE_ACTIVE                = 0;
constexpr uint32_t TASK_CLEANUP_STAGE_EXIT_REQUESTED        = 1;
constexpr uint32_t TASK_CLEANUP_STAGE_SCHEDULER_DETACHED    = 2;
constexpr uint32_t TASK_CLEANUP_STAGE_WAITING_FOR_TLB_SYNC  = 3;
constexpr uint32_t TASK_CLEANUP_STAGE_READY_TO_RECLAIM      = 4;

/**
 * Per-task TLB sync ticket used by reaper before reclaiming task stacks.
 *
 * The ticket snapshots each CPU's reclaim epoch and requires every CPU to
 * advance past that snapshot before stack unmap/free can proceed. It only
 * retires stale TLB entries for the freed stacks, keeping the task struct
 * itself alive is the job of its counted references.
 */
struct task_tlb_sync_ticket {
    uint64_t cpu_epoch_snapshot[MAX_CPUS];
    sync::atomic<uint32_t> armed;
};

struct thread_group;

/**
 * A schedulable unit of execution. Refcounted: a task starts with one
 * reference, dropped once the scheduler has detached it after death, and
 * the last release hands reclamation to the reaper.
 */
struct task : rc::ref_counted<task> {
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
    sync::atomic<uint32_t> state;
    sync::atomic<uint32_t> cleanup_stage;

    // Thread id address registered by a cloned thread, exit writes
    // zero there and wakes one futex waiter so pthread_join returns
    uintptr_t      clear_child_tid;

    // Stacks
    uintptr_t      task_stack_base;
    uintptr_t      sys_stack_base;

    // Signals (per-thread blocked mask and pending set). A
    // pending SIGKILL bit is the task's "kill pending" marker.
    signals::task_signals sig;

    // Scheduler state
    list::node              sched_link;
    list::node              wait_link;
    list::node              timer_link;
    uint64_t                timer_deadline;
    sync::atomic<uint64_t>  run_ticks; // timer ticks observed while current
    task_tlb_sync_ticket    tlb_sync_ticket;
    rc::reaper::dead_node   reaper_node;

    // Resources, the handle table is private by default and shared
    // when a thread is created with POSIX file table semantics
    resource::handle_table* handles;
    resource::proc_provider::proc_resource* proc_res;

    /**
     * Defers reclamation to the reaper, which owns the staged teardown
     * and the TLB grace period for stack pages.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static void ref_destroy(task* self);
};

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
    uint32_t       pid; // process leader tid
    sync::atomic<uint32_t> group_id; // process group this process belongs to
    list::head<task, &task::group_link> threads; // non-leader threads only
    uint32_t       thread_count; // number of live non-leader threads

    // Group exit status recorded by exit_group: zero means unset, otherwise bit
    // 31 is set and bits 8 to 15 hold the exit code as a normal wait status
    sync::atomic<uint32_t> group_exit_status;

    // Signals (per-process action table and shared pending set)
    signals::group_signals sig;

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static void ref_destroy(thread_group* self);
};

} // namespace sched

#endif // STELLUX_SCHED_TASK_H
