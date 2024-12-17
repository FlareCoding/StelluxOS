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
 */
__PRIVILEGED_CODE
task_control_block* get_idle_task(uint64_t cpu);

__PRIVILEGED_CODE void install_sched_irq_handlers();

class scheduler {
public:
    __PRIVILEGED_CODE static scheduler& get();

    __PRIVILEGED_CODE void init();
    __PRIVILEGED_CODE void register_cpu_run_queue(uint64_t cpu);
    __PRIVILEGED_CODE void unregister_cpu_run_queue(uint64_t cpu);

    __PRIVILEGED_CODE void add_task(task_control_block* task, int cpu = -1);
    __PRIVILEGED_CODE void remove_task(task_control_block* task);

    // Called from the IRQ interrupt context. Picks the
    // next task to run and switches the context into it.
    __PRIVILEGED_CODE void __schedule(ptregs* irq_frame);

    // Forces a new task to get scheduled and triggers a
    // context switch without the need for a timer tick.
    __PRIVILEGED_CODE void schedule();

    // Masks timer tick-based interrupts
    __PRIVILEGED_CODE void preempt_disable(int cpu = -1);

    // Unmasks timer tick-based interrupts
    __PRIVILEGED_CODE void preempt_enable(int cpu = -1);

private:
    kstl::shared_ptr<sched_run_queue> m_run_queues[MAX_SYSTEM_CPUS];

    int _load_balance_find_cpu();
};
} // namespace sched

#endif // SCHED_H
