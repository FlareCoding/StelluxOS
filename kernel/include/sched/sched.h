#ifndef SCHED_H
#define SCHED_H
#include "run_queue.h"
#include <memory/memory.h>

namespace sched {
/**
 * @brief Retrieves the idle task for a specified CPU.
 * 
 * This function returns a pointer to the idle task control block associated with the given CPU.
 * The idle task is a special task that runs when no other runnable tasks are available on the CPU.
 * It typically performs low-priority operations such as conserving power or maintaining system stability.
 * 
 * @param cpu The identifier of the CPU for which to retrieve the idle task. This should correspond
 *            to a valid CPU index within the system.
 * @return task_control_block* A pointer to the idle task's control block for the specified CPU.
 *                             Returns nullptr if the idle task for the given CPU does not exist
 *                             or if the CPU identifier is invalid.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE task_control_block* get_idle_task(uint64_t cpu);

/**
 * @brief Installs the IRQ handlers for the scheduler.
 * 
 * Configures the necessary interrupt handlers to support scheduling functionality,
 * including context switching and task management during interrupts.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void install_sched_irq_handlers();

/**
 * @class scheduler
 * @brief Manages CPU scheduling and task execution.
 * 
 * Coordinates task queues, CPU assignments, and context switching to ensure efficient 
 * task scheduling across all available CPUs.
 */
class scheduler {
public:
    /**
     * @brief Retrieves the singleton instance of the scheduler.
     * @return Reference to the singleton instance of the `scheduler`.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static scheduler& get();

    /**
     * @brief Initializes the scheduler.
     * 
     * Prepares the scheduler for operation, including setting up run queues and idle tasks.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void init();

    /**
     * @brief Registers a run queue for a specific CPU.
     * @param cpu The CPU for which to register a run queue.
     * 
     * Prepares the CPU to manage tasks through its dedicated run queue.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void register_cpu_run_queue(uint64_t cpu);

    /**
     * @brief Unregisters a run queue for a specific CPU.
     * @param cpu The CPU for which to unregister the run queue.
     * 
     * Cleans up the run queue for the specified CPU, preventing further task scheduling on it.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void unregister_cpu_run_queue(uint64_t cpu);

    /**
     * @brief Adds a task to a CPU's run queue.
     * @param task Pointer to the task control block to add.
     * @param cpu The CPU to which the task should be assigned. Defaults to -1 (automatic CPU selection).
     * 
     * Enqueues the task for execution, either on the specified CPU or the least-loaded CPU if `cpu = -1`.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void add_task(task_control_block* task, int cpu = -1);

    /**
     * @brief Removes a task from its CPU's run queue.
     * @param task Pointer to the task control block to remove.
     * 
     * Dequeues the task, ensuring it is no longer eligible for scheduling.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void remove_task(task_control_block* task);

    /**
     * @brief Selects the next task to run and switches to it.
     * @param irq_frame Pointer to the interrupt frame from which the context switch is initiated.
     * 
     * Called from IRQ context to select the next runnable task and perform a context switch.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void __schedule(ptregs* irq_frame);

    /**
     * @brief Triggers a context switch without requiring a timer interrupt.
     * 
     * Forces the scheduler to pick a new task to run and switches to it. Can be called by
     * userspace for yielding purposes.
     */
    void schedule();

    /**
     * @brief Disables preemption by masking timer tick interrupts.
     * @param cpu The CPU for which to disable preemption. Defaults to -1 (current CPU).
     * 
     * Disables preemption to prevent context switches.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void preempt_disable(int cpu = -1);

    /**
     * @brief Enables preemption by unmasking timer tick interrupts.
     * @param cpu The CPU for which to enable preemption. Defaults to -1 (current CPU).
     * 
     * Restores preemption, allowing context switches to occur.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void preempt_enable(int cpu = -1);

private:
    kstl::shared_ptr<sched_run_queue> m_run_queues[MAX_SYSTEM_CPUS];

    /**
     * @brief Finds the least-loaded CPU for task assignment.
     * @return The CPU ID of the least-loaded CPU.
     * 
     * Used internally to balance tasks across CPUs.
     */
    int _load_balance_find_cpu();
};
} // namespace sched

#endif // SCHED_H
