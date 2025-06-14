#ifndef SCHED_H
#define SCHED_H
#include "run_queue.h"
#include <memory/memory.h>

namespace sched {
/**
 * @brief Retrieves the idle process core for a specified CPU.
 * 
 * This function returns a pointer to the idle process core associated with the given CPU.
 * The idle process core contains the execution state for the idle process.
 * 
 * @param cpu The identifier of the CPU for which to retrieve the idle process core.
 * @return process_core* A pointer to the idle process core, or nullptr if invalid CPU.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE process_core* get_idle_process_core(uint64_t cpu);

/**
 * @brief Retrieves the idle process for a specified CPU.
 * 
 * This function returns a pointer to the idle process associated with the given CPU.
 * The idle process is a special process that runs when no other runnable processes are available.
 * 
 * @param cpu The identifier of the CPU for which to retrieve the idle process.
 * @return process* A pointer to the idle process, or nullptr if invalid CPU.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE process* get_idle_process(uint64_t cpu);

/**
 * @brief Installs the IRQ handlers for the scheduler.
 * 
 * Configures the necessary interrupt handlers to support scheduling functionality,
 * including context switching and process management during interrupts.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void install_sched_irq_handlers();

/**
 * @class scheduler
 * @brief Manages CPU scheduling and process execution.
 * 
 * Coordinates process queues, CPU assignments, and context switching to ensure efficient 
 * process scheduling across all available CPUs.
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
     * Prepares the scheduler for operation, including setting up run queues and idle processes.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void init();

    /**
     * @brief Registers a run queue for a specific CPU.
     * @param cpu The CPU for which to register a run queue.
     * 
     * Prepares the CPU to manage processes through its dedicated run queue.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void register_cpu_run_queue(uint64_t cpu);

    /**
     * @brief Unregisters a run queue for a specific CPU.
     * @param cpu The CPU for which to unregister the run queue.
     * 
     * Cleans up the run queue for the specified CPU, preventing further process scheduling on it.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void unregister_cpu_run_queue(uint64_t cpu);

    /**
     * @brief Adds a process to a CPU's run queue.
     * @param proc Pointer to the process to add.
     * @param cpu The CPU to which the process should be assigned. Defaults to -1 (automatic CPU selection).
     * 
     * Enqueues the process for execution, either on the specified CPU or the least-loaded CPU if `cpu = -1`.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void add_process(process* proc, int cpu = -1);

    /**
     * @brief Removes a process from its CPU's run queue.
     * @param proc Pointer to the process to remove.
     * 
     * Dequeues the process, ensuring it is no longer eligible for scheduling.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void remove_process(process* proc);

    /**
     * @brief Selects the next process to run and switches to it.
     * @param irq_frame Pointer to the interrupt frame from which the context switch is initiated.
     * 
     * Called from IRQ context to select the next runnable process and perform a context switch.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void __schedule(ptregs* irq_frame);

    /**
     * @brief Triggers a context switch without requiring a timer interrupt.
     * 
     * Forces the scheduler to pick a new process to run and switches to it. Can be called by
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
     * @brief Finds the least-loaded CPU for process assignment.
     * @return The CPU ID of the least-loaded CPU.
     * 
     * Used internally to balance processes across CPUs.
     */
    int _load_balance_find_cpu();
};
} // namespace sched

#endif // SCHED_H
