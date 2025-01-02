#include <sched/run_queue.h>

namespace sched {
sched_run_queue::sched_run_queue() : m_next_index(0) {}

void sched_run_queue::add_task(task_control_block* task) {
    mutex_guard guard(m_lock); // Automatically acquires and releases the lock
    m_tasks.push_back(task);
}

void sched_run_queue::remove_task(task_control_block* task) {
    mutex_guard guard(m_lock);

    // Search for the task and remove it
    for (size_t i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks[i] == task) {
            m_tasks.erase(i);

            // Adjust the round-robin index if needed
            if (i < m_next_index && m_next_index > 0) {
                --m_next_index;
            } else if (m_next_index >= m_tasks.size()) {
                m_next_index = 0;
            }

            return;
        }
    }
}

task_control_block* sched_run_queue::pick_next() {
    mutex_guard guard(m_lock);

    if (m_tasks.empty()) {
        return nullptr; // No task to run
    }

    size_t original_index = m_next_index;

    // Attempt to find a non-idle task
    do {
        task_control_block* next_task = m_tasks[m_next_index];
        m_next_index = (m_next_index + 1) % m_tasks.size();

        // Check if this is a non-idle task
        if (next_task->pid != 0) { // Assuming idle task has pid 0
            return next_task;
        }

    } while (m_next_index != original_index); // Ensure we loop back to the original position

    // If we only have the idle task, return it
    return m_tasks[original_index];
}

bool sched_run_queue::is_empty() {
    mutex_guard guard(m_lock);
    return m_tasks.empty();
}
} // namespace sched
