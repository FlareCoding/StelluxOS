#ifndef STELLUX_SCHED_SCHED_H
#define STELLUX_SCHED_SCHED_H

#include "common/types.h"

namespace sched {

struct task;

constexpr int32_t OK         = 0;
constexpr int32_t ERR_NO_MEM = -1;

/**
 * @brief Initialize the scheduler for the BSP. Creates idle task,
 * per-CPU runqueue, and scheduling policy. Call after mm::init().
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Initialize the scheduler for an AP. Creates idle task and
 * per-CPU runqueue. Must be called after percpu::init_ap().
 * @param cpu_id Logical CPU ID of the AP.
 * @param task_stack_top Top of the AP's task stack.
 * @param system_stack_top Top of the AP's system stack (separate from task stack).
 * @return OK on success, ERR_NO_MEM on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_ap(uint32_t cpu_id, uintptr_t task_stack_top,
                                  uintptr_t system_stack_top);

/**
 * @brief Create a new kernel task. Allocates task struct and stacks.
 * Returns in TASK_STATE_CREATED (not yet enqueued).
 * @param entry Task entry function.
 * @param arg Argument passed to entry via first register.
 * @param name Debug name (not copied, caller must ensure lifetime).
 * @param flags Optional task flags. Default (0) creates a lowered task
 *   (Ring 3 / EL0) with unprivileged stacks. Pass TASK_FLAG_ELEVATED
 *   to create an elevated task (Ring 0 / EL1) with privileged stacks.
 * @return task pointer on success, nullptr on failure.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE
task* create_kernel_task(void (*entry)(void*), void* arg, const char* name,
                         uint32_t flags = 0);

/**
 * @brief Add a task to the local CPU's runqueue.
 * Atomically transitions the task from CREATED to READY via CAS.
 * Rejects tasks that are already enqueued, running, or dead.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void enqueue(task* t);

/**
 * @brief Add a task to a specific CPU's runqueue.
 * Same semantics as enqueue() but targets a remote CPU. The target
 * CPU's timer tick will pick up the task within one scheduling period.
 * @param t Task in TASK_STATE_CREATED.
 * @param cpu_id Logical CPU ID to enqueue on.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void enqueue_on(task* t, uint32_t cpu_id);

/**
 * @brief Resume a blocked task by placing it on the local runqueue.
 * Atomically transitions BLOCKED -> READY via CAS.
 * Called by sync::wake_one / sync::wake_all.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void wake(task* t);

/**
 * @brief Yield the current CPU to the scheduler (cooperative switch).
 * Triggers a software interrupt that routes through the trap path.
 */
void yield();

/**
 * @brief Terminate the current task. Marks it DEAD and yields.
 * Developer must call this explicitly before returning from task entry.
 * @param exit_code Exit code to return to the parent task.
 */
[[noreturn]] void exit(int exit_code);

/**
 * @brief Get the current task on this CPU.
 */
task* current();

} // namespace sched

#endif // STELLUX_SCHED_SCHED_H
