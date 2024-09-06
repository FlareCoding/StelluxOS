#ifndef SCHED_H
#define SCHED_H
#include <arch/x86/per_cpu_data.h>
#include <process/process.h>

#define MAX_QUEUED_PROCESSES 128

EXTERN_C PCB g_kernelSwapperTasks[MAX_CPUS];

class RoundRobinScheduler {
public:
    RoundRobinScheduler();
    ~RoundRobinScheduler() = default;

    static RoundRobinScheduler& get();

    inline PCB* getCurrentTask() { return &m_runQueue[m_currentTaskIndex]; }
    PCB* peekNextTask();
    
    inline size_t getQueuedTaskCount() const { return m_tasksInQueue; }

    bool switchToNextTask();

    size_t addTask(const PCB& task);
    PCB* getTask(size_t idx);
    PCB* findTaskByPid(pid_t pid);
    void removeTask(pid_t pid);

private:
    PCB m_runQueue[MAX_QUEUED_PROCESSES];

    size_t m_tasksInQueue = 0;
    
    size_t m_currentTaskIndex = 0;
    size_t m_nextTaskIndex = 0;
};

//
// Allows the current running kernel thread to terminate and switch to the next
// available task without waiting for the next timer interrupt. If no next valid
// task is available, control flow switches back to the kernel swapper task.
//
void exitKernelThread();

#endif
