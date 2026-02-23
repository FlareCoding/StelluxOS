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
 * to trap exit. Updates TSS.RSP0 on x86 (no-op on aarch64).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void arch_post_switch(task* next);

/**
 * Common: called by arch on_yield/on_tick handler. Handles runqueue lock,
 * state transitions, and picking the next task.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE task* pick_next_and_switch(task* prev);

} // namespace sched

#endif // STELLUX_SCHED_SCHED_INTERNAL_H
