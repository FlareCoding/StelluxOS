#ifndef STELLUX_SCHED_SCHED_H
#define STELLUX_SCHED_SCHED_H

#include "common/types.h"
#include "rc/strong_ref.h"

namespace exec { struct loaded_image; }

namespace sched {

struct task;

constexpr int32_t OK         = 0;
constexpr int32_t ERR_NO_MEM = -1;

// Exec-style string limits shared by proc_create copying and the user stack builder
constexpr size_t MAX_ARG_STRLEN  = 256; // bytes per argv/envp string, including NUL
constexpr size_t MAX_ARG_STRINGS = 64; // strings per argv/envp array

/**
 * One CPU's timer tick counters. A tick is charged as idle when it
 * interrupts the CPU's idle task and as busy otherwise. Snapshots
 * carry the scheduler tick frequency so readers can convert tick
 * counts into wall time.
 */
struct cpu_accounting_stats {
    uint64_t busy_ticks;
    uint64_t idle_ticks;
    uint32_t tick_hz;
};

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
 * @param envc Number of environment strings.
 * @param envp Array of kernel-copied environment strings, or nullptr for none.
 * @return task pointer on success, nullptr on failure.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE
task* create_user_task(exec::loaded_image* image, const char* name,
                       int argc = 0, const char* const* argv = nullptr,
                       int envc = 0, const char* const* envp = nullptr);

/**
 * @brief Create a new user thread in an existing user process.
 * The thread shares the creator's address space (mm_context) and joins its
 * thread_group. System stack is allocated in kernel VA, the caller supplies
 * the user stack. Returns in TASK_STATE_CREATED (not yet enqueued).
 * @param creator Any task in the target process, its resource handle table
 *   and mm_context are inherited by the new thread.
 * @param entry User-space entry point address.
 * @param arg Argument passed to entry via first register.
 * @param stack_top Top of the caller-allocated user stack in the shared
 *   address space.
 * @param name Debug name (copied into embedded task storage).
 * @return task pointer on success, nullptr on failure.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE
task* create_user_thread(task* creator, uintptr_t entry, uintptr_t arg,
                        uintptr_t stack_top, const char* name);

/**
 * @brief Create a thread for the musl pthread clone path.
 * Unlike create_user_thread, the child resumes at the caller's
 * syscall return point with a zero return value instead of entering
 * a function, which is what the musl thread stub expects. Must be
 * called from syscall context so the caller's saved user register
 * frame is available to seed the child context.
 * @param creator The calling task, must be the current task.
 * @param stack_top Child user stack pointer, used exactly as passed.
 * @param tls New thread pointer value when set_tls is true.
 * @param set_tls True when the caller requested a new thread pointer.
 * @param share_files True shares the caller's handle table, false
 *   snapshots it with native copy semantics.
 * @return task pointer on success, nullptr on failure.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE
task* clone_user_thread(task* creator, uintptr_t stack_top, uintptr_t tls,
                        bool set_tls, bool share_files);

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
 * @brief Resume a blocked task by placing it on its CPU's runqueue.
 * Atomically transitions BLOCKED -> READY via CAS.
 * Called by sync::wake_one / sync::wake_all.
 *
 * The caller must pin t so the reaper cannot free it mid-call: hold a
 * counted reference (task_ref) or a lock t must take before it can exit.
 * A remote wake spins until t leaves its CPU, so never hold a spinlock
 * with interrupts off across the call.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void wake(task* t);

/**
 * @brief Mark a task for termination and wake it if blocked.
 * Fire-and-forget: the target is force-woken now or observes the kill
 * at its next killable blocking attempt (sleep, futex, poll).
 * Same pin and spin rules as wake.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void force_wake_for_kill(task* t);

/**
 * @brief Acquire a counted reference to a task from a raw pointer.
 * The raw pointer must still be protected here: hold a lock the task must
 * take before it can finish exiting, or another counted reference. Returns
 * a null reference if the task is already tearing down.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE rc::strong_ref<task> task_ref(task* t);

/**
 * @brief Acquire a counted reference to the task with the given tid.
 * Takes the registry lock internally, so no caller-side pin is needed.
 * Returns a null reference if no task with that tid is registered or if
 * the task is already tearing down.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE rc::strong_ref<task> task_ref_by_tid(uint32_t tid);

/**
 * @brief Publish intent to block: moves the current task to BLOCKED.
 * Pair with block_task_interrupted before yielding.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void prepare_to_block_task();

/**
 * @brief True if a kill or fatal signal arrived since prepare_to_block_task.
 * On true, the caller must unwind its wait entry and call cancel_block_task.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool block_task_interrupted();

/**
 * @brief Revert an unfinished block after the caller unwound its entry.
 * Yields once if a concurrent wake already claimed the task.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void cancel_block_task();

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
 * @brief Read a snapshot of a CPU's accounting stats. Safe to call
 * from any CPU.
 * @param cpu_id Logical CPU ID, must be below smp::cpu_count().
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE cpu_accounting_stats read_cpu_accounting_stats(uint32_t cpu_id);

/**
 * @brief Block the current task for at least ns nanoseconds.
 * The task is placed on the per-CPU sleep queue and woken by the
 * timer interrupt when the deadline expires.
 * Must not be called from the idle task.
 * @param ns Duration in nanoseconds. If 0, yields without blocking.
 * @return Nanoseconds left if interrupted by a kill or fatal signal,
 *   0 if the full duration elapsed.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint64_t sleep_ns(uint64_t ns);

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
