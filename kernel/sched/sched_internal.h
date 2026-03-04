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
 * Ownership boundary:
 * - Updates task scheduler ownership (current_task/current_task_exec).
 * - Must NOT finalize per-CPU runtime elevation state for trap/syscall return.
 *   Trap/syscall return-boundary code restores percpu_is_elevated from the
 *   selected task's TASK_FLAG_ELEVATED after switch teardown is complete.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE task* pick_next_and_switch(task* prev);

/**
 * Common: publish on_cpu=0 for a previously switched-out task.
 * Must be called from arch scheduler trap paths before taking reaper decisions.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void finalize_pending_off_cpu();

/**
 * Common: defer on_cpu publication for the task switched out in this trap.
 * Call only after switch-out work (including FPU save/restore) is complete.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void defer_off_cpu_finalize(task* prev);

/**
 * Common: advances this CPU's TLB sync epoch.
 * This marks a safe point that reaper can rely on before reclaiming stack pages.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void advance_cpu_tlb_sync_epoch();

} // namespace sched

#endif // STELLUX_SCHED_SCHED_INTERNAL_H
