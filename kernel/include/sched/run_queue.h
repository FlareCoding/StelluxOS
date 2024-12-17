#ifndef RUN_QUEUE_H
#define RUN_QUEUE_H
#include <sync.h>
#include <process/process.h>
#include <kstl/vector.h>

class sched_run_queue {
public:
    sched_run_queue();

    // Add a task to the queue
    void add_task(task_control_block* task);

    // Remove a specific task from the queue
    void remove_task(task_control_block* task);

    // Pick the next task to run (based on a simple Round-Robin for now)
    task_control_block* pick_next();

    // Check if the queue is empty
    bool is_empty();

private:
    kstl::vector<task_control_block*> m_tasks;
    mutex m_lock = mutex();
    size_t m_next_index;
};

#endif // RUN_QUEUE_H
