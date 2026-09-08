#ifndef STELLUX_SCHED_SCHED_INTERNAL_H
#define STELLUX_SCHED_SCHED_INTERNAL_H

#include "sched/task.h"

namespace sched {

/**
 * Arch-specific: fill cpu_ctx for a new task's initial state.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void arch_init_task_context(
    task* t, void (*entry)(void*), void* arg);

/**
 * Arch-specific: fill cpu_ctx for a cloned thread from the calling
 * task's saved syscall register frame. The child gets a zero syscall
 * return value and the stack pointer from exec.task_stack_top.
 * Must run in the creator's syscall context.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void arch_init_clone_cpu_context(task* t);

/**
 * Arch-specific: called after picking the next task, before returning
 * to trap exit. Updates architecture-specific post-switch state
 * (e.g. TSS.RSP0 on x86, translation roots on aarch64).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void arch_post_switch(task* next);

/**
 * Common: called by arch on_yield/on_tick handler. Handles runqueue lock,
 * state transitions, and picking the next task.
 *
 * preempted is true from the tick and false from a yield. Only a yield may
 * leave a BLOCKED prev off the queue, since that is the task's own decision to
 * block. A tick between prepare_to_block_task and that yield keeps it queued.
 *
 * Ownership boundary:
 * - Updates task scheduler ownership (current_task/current_task_exec) and
 *   marks next on-CPU under the runqueue lock, so a waker holding that lock
 *   never sees a task that is neither queued nor on-CPU yet.
 * - Must NOT finalize per-CPU runtime elevation state for trap/syscall return.
 *   Trap/syscall return-boundary code restores percpu_is_elevated from the
 *   selected task's TASK_FLAG_ELEVATED after switch teardown is complete.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE task* pick_next_and_switch(task* prev, bool preempted);

/**
 * Common: charge one timer tick to the interrupted task and to this
 * CPU's busy or idle counter. Called by arch on_tick handlers before
 * any early return so non-preemptible work is still recorded.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void record_cpu_tick(task* prev);

/**
 * Common: the first code after a task switch. The trap exit stub calls it on
 * the per-CPU exit stack with interrupts masked once nothing on this CPU
 * reads prev's stack anymore, and it publishes prev off-CPU state. The exit
 * stack is small and very sensitive, so it must not fault, block, or log.
 * @note Privilege: **required**
 */
extern "C" __PRIVILEGED_CODE void stlx_finish_task_switch(task_exec_core* prev);

} // namespace sched

#endif // STELLUX_SCHED_SCHED_INTERNAL_H
