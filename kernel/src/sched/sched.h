#ifndef SCHED_H
#define SCHED_H
#include <core/kvector.h>
#include <arch/x86/per_cpu_data.h>
#include <process/process.h>
#include <sync.h>

using Task = PCB;

EXTERN_C Task g_kernelSwapperTasks[MAX_CPUS];

struct SchedulerRunQueue {
    DECLARE_SPINLOCK(lock);
    size_t currentTaskIndex;
    kstl::vector<Task*> tasks;
};

class Scheduler {
public:
    Scheduler() = default;
    ~Scheduler() = default;

    static Scheduler& get();

    void init();
    void registerCoreForScheduling(int cpu);

    // Get the current running task
    Task* getCurrentTask(int cpu);

    // Look at the next available task
    Task* peekNextTask(int cpu);

    // Schedule the next task to run
    void scheduleNextTask(int cpu);

    // Add a task to a specified cpu core
    void addTaskToCpu(Task* task, int cpu);

    // Adds a task to the next least loaded available cpu core
    void addTask(Task* task);

    // Remove a task
    void removeTaskFromCpu(int pid, int cpu);

private:
    kstl::vector<kstl::SharedPtr<SchedulerRunQueue>> m_runQueues;

    int _findLeastLoadedCpu();
    size_t _getRunQueueSize(size_t cpu);
};

//
// Allocates a task object for a new kernel thread that will
// start its execution at a given function in userspace (DPL=3).
//
Task* createKernelTask(void (*taskEntry)(), int priority = 0);

//
// Destroys a task object, releasing any resources allocated for the task.
// This function should properly clean up any state or memory associated 
// with the task, ensuring it no longer runs and freeing up any used memory.
//
// Parameters:
// - task: A pointer to the Task object to be destroyed.
//         The Task pointer must not be used after calling this function.
//
// Returns:
// - Returns true if the task was successfully destroyed. False if there
//   was an error (such as the task not being found).
//
bool destroyKernelTask(Task* task);

//
// Allows the current running kernel thread to terminate and switch to the next
// available task without waiting for the next timer interrupt. If no next valid
// task is available, control flow switches back to the kernel swapper task.
//
void exitKernelThread();

#endif
