#ifndef STELLUX_SCHED_SCHED_H
#define STELLUX_SCHED_SCHED_H

#include "common/types.h"

namespace exec { struct loaded_image; }

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
 * @brief Create a new user task from a loaded ELF image.
 * Allocates a user stack in the user page table and a system stack in kernel VA.
 * Returns in TASK_STATE_CREATED (not yet enqueued).
 * @param image Loaded ELF image with entry point and mm context ownership.
 * @param name Debug name (copied into embedded task storage).
 * @param argc Number of user-provided arguments (excluding program name).
 * @param argv Array of kernel-copied argument strings, or nullptr for none.
 * @return task pointer on success, nullptr on failure.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE
task* create_user_task(exec::loaded_image* image, const char* name,
                       int argc = 0, const char* const* argv = nullptr);

/**
 * @brief Add a task to a runqueue, distributing across CPUs via round-robin.
 * Atomically transitions the task from CREATED to READY via CAS.
 * Rejects tasks that are already enqueued, running, or dead.
 * Use enqueue_on() to target a specific CPU instead.
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
 * @brief Mark a task for termination and wake it if blocked.
 * Sets kill_pending, cancels any timer sleep, and wakes via CAS.
 * Fire-and-forget: does not wait for the task to actually die.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void force_wake_for_kill(task* t);

/**
 * @brief Check if the current task has been marked for termination.
 * Safe to call from any kernel context with a valid per-CPU base.
 */
bool is_kill_pending();

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

/**
 * @brief Block the current task for at least ns nanoseconds.
 * The task is placed on the per-CPU sleep queue and woken by the
 * timer interrupt when the deadline expires.
 * Must not be called from the idle task.
 * @param ns Duration in nanoseconds. If 0, yields without blocking.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void sleep_ns(uint64_t ns);

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void sleep_us(uint64_t us);

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void sleep_ms(uint64_t ms);

} // namespace sched

#endif // STELLUX_SCHED_SCHED_H
