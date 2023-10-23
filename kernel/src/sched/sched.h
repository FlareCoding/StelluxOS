#ifndef SCHED_H
#define SCHED_H
#include <process/process.h>

#define MAX_QUEUED_PROCESSES 64

class Scheduler {
public:
    Scheduler() = default;
    static Scheduler& get();

    void init();

    // Returns the index of the task in the queue
    int32_t addTask(PCB task);

    // Removes the task from the queue with a given index
    void removeTask(int32_t index);

    // Removes the task from the queue with a given PID
    void removeTaskWithPid(uint64_t pid);

    // Returns the state of a given task at index
    ProcessState getTaskState(int32_t index);

    // Uses a simple Round-Robin algorithm
    // Returns nullptr if no next task is available to run
    PCB* getNextTask();

    // Returns the current task in the queue (could be inactive)
    inline PCB* getCurrentTask() { return &m_taskQueue[m_currentTaskIndex]; }

private:
    PCB m_taskQueue[MAX_QUEUED_PROCESSES];
    uint64_t m_taskCount = 0;
    uint64_t m_currentTaskIndex = 0;
};

#endif
