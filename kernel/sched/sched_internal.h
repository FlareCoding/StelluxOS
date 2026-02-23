#ifndef STELLUX_SCHED_SCHED_INTERNAL_H
#define STELLUX_SCHED_SCHED_INTERNAL_H

#include "sched/task.h"

namespace sched {

// Arch-specific: fill cpu_ctx for a new task's initial state.
// Sets IP=entry, arg register=arg, SP=stack top, IF=enabled.
// Reads t->exec.flags to determine privilege level (elevated or lowered).
__PRIVILEGED_CODE void arch_init_task_context(
    task* t, void (*entry)(void*), void* arg);

// Arch-specific: called after picking the next task, before returning
// to trap exit. Updates TSS.RSP0 on x86 (no-op on aarch64).
__PRIVILEGED_CODE void arch_post_switch(task* next);

// Common: called by arch on_yield handler. Handles runqueue lock,
// state transitions, and picking the next task.
// Returns the next task to run (may be prev itself if nothing else is ready).
__PRIVILEGED_CODE task* pick_next_and_switch(task* prev);

} // namespace sched

#endif // STELLUX_SCHED_SCHED_INTERNAL_H
