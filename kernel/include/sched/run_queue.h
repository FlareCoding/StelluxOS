#ifndef RUN_QUEUE_H
#define RUN_QUEUE_H
#include <sync.h>
#include <process/process.h>
#include <kstl/vector.h>

namespace sched {
/**
 * @class sched_run_queue
 * @brief Represents a process run queue for CPU scheduling.
 * 
 * Maintains a queue of processes eligible for execution on a CPU and provides methods to add, remove,
 * and select processes using a Round-Robin scheduling policy.
 */
class sched_run_queue {
public:
    /**
     * @brief Constructs an empty run queue.
     */
    sched_run_queue();

    /**
     * @brief Adds a process to the run queue.
     * @param process Pointer to the process to add.
     * 
     * Enqueues the process for scheduling. This method is thread-safe and ensures
     * proper synchronization when modifying the queue.
     */
    void add_process(process* proc);

    /**
     * @brief Removes a specific process from the run queue.
     * @param process Pointer to the process to remove.
     * 
     * Dequeues the specified process, making it ineligible for further scheduling.
     * This method is thread-safe and ensures proper synchronization.
     */
    void remove_process(process* proc);

    /**
     * @brief Picks the next process to run using a Round-Robin policy.
     * @return Pointer to the next process, or `nullptr` if the queue is empty.
     * 
     * Selects the next process for execution based on a simple Round-Robin scheduling algorithm.
     * This method ensures fairness by cycling through processes in the order they were added.
     */
    process* pick_next();

    /**
     * @brief Checks if the run queue is empty.
     * @return True if the queue is empty, false otherwise.
     */
    bool is_empty();

    /**
     * @brief Retrieves the size of the run queue.
     * @return The number of processes currently in the queue.
     */
    size_t size() const { return m_processes.size(); }

private:
    kstl::vector<process*> m_processes;
    mutex m_lock = mutex();
    size_t m_next_index;
};
} // namespace sched

#endif // RUN_QUEUE_H
