#ifndef SCHED_H
#define SCHED_H
#include <process/process.h>

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
} // namespace sched

#endif // SCHED_H
