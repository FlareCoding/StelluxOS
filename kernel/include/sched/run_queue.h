#ifndef RUN_QUEUE_H
#define RUN_QUEUE_H
#include <sync.h>
#include <process/process.h>
#include <kstl/vector.h>

namespace sched {
/**
 * @class sched_run_queue
 * @brief Represents a task run queue for CPU scheduling.
 * 
 * Maintains a queue of tasks eligible for execution on a CPU and provides methods to add, remove,
 * and select tasks using a Round-Robin scheduling policy.
 */
class sched_run_queue {
public:
    /**
     * @brief Constructs an empty run queue.
     */
    sched_run_queue();

    /**
     * @brief Adds a task to the run queue.
     * @param task Pointer to the task control block to add.
     * 
     * Enqueues the task for scheduling. This method is thread-safe and ensures
     * proper synchronization when modifying the queue.
     */
    void add_task(task_control_block* task);

    /**
     * @brief Removes a specific task from the run queue.
     * @param task Pointer to the task control block to remove.
     * 
     * Dequeues the specified task, making it ineligible for further scheduling.
     * This method is thread-safe and ensures proper synchronization.
     */
    void remove_task(task_control_block* task);

    /**
     * @brief Picks the next task to run using a Round-Robin policy.
     * @return Pointer to the next task control block, or `nullptr` if the queue is empty.
     * 
     * Selects the next task for execution based on a simple Round-Robin scheduling algorithm.
     * This method ensures fairness by cycling through tasks in the order they were added.
     */
    task_control_block* pick_next();

    /**
     * @brief Checks if the run queue is empty.
     * @return True if the queue is empty, false otherwise.
     */
    bool is_empty();

    /**
     * @brief Retrieves the size of the run queue.
     * @return The number of tasks currently in the queue.
     */
    size_t size() const { return m_tasks.size(); }

private:
    kstl::vector<task_control_block*> m_tasks;
    mutex m_lock = mutex();
    size_t m_next_index;
};
} // namespace sched

#endif // RUN_QUEUE_H
