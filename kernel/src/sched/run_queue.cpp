#include <sched/run_queue.h>

namespace sched {
sched_run_queue::sched_run_queue() : m_next_index(0) {}

void sched_run_queue::add_process(process* proc) {
    mutex_guard guard(m_lock); // Automatically acquires and releases the lock
    m_processes.push_back(proc);
}

void sched_run_queue::remove_process(process* proc) {
    mutex_guard guard(m_lock);

    // Search for the process and remove it
    for (size_t i = 0; i < m_processes.size(); ++i) {
        if (m_processes[i] == proc) {
            m_processes.erase(i);

            // Adjust the round-robin index if needed
            if (i < m_next_index && m_next_index > 0) {
                --m_next_index;
            } else if (m_next_index >= m_processes.size()) {
                m_next_index = 0;
            }

            return;
        }
    }
}

process* sched_run_queue::pick_next() {
    mutex_guard guard(m_lock);

    if (m_processes.empty()) {
        return nullptr; // No process to run
    }

    size_t original_index = m_next_index;

    // Attempt to find a non-idle process
    do {
        process* next_proc = m_processes[m_next_index];
        m_next_index = (m_next_index + 1) % m_processes.size();

        process_core* next_proc_core = next_proc->get_core();

        // Check if this is a non-idle process
        if (next_proc_core && next_proc_core->identity.pid != 0 && next_proc_core->state == process_state::READY) { // Assuming idle process has pid 0
            return next_proc;
        }

    } while (m_next_index != original_index); // Ensure we loop back to the original position

    // If we only have the idle process, return it
    return m_processes[original_index];
}

bool sched_run_queue::is_empty() {
    mutex_guard guard(m_lock);
    return m_processes.empty();
}
} // namespace sched
