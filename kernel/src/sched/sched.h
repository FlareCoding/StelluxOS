#ifndef SCHED_H
#define SCHED_H
#include <core/kvector.h>
#include <arch/x86/per_cpu_data.h>
#include <process/process.h>

#define MAX_QUEUED_PROCESSES 128

using Task = PCB;

EXTERN_C Task g_kernelSwapperTasks[MAX_CPUS];

class RoundRobinRunQueue {
public:
    RoundRobinRunQueue() = default;
    ~RoundRobinRunQueue() = default;

    size_t size() const;

    // Adds a task to the run queue
    bool addTask(Task* task);

    // Removes a task with a specified
    // pid from the run queue if it exists.
    bool removeTask(pid_t pid);

    // Returns a pointer to the current scheduled task.
    // There should be guaranteed to always be at least
    // one task in the run queue (kernel swapper task).
    Task* getCurrentTask();

    // Returns the next available schedulable task in the run queue
    Task* peekNextTask();

    // Takes the current running task off the run
    // queue and schedules the next available task.
    void scheduleNextTask();

private:
    // Run queue of tasks
    kstl::vector<Task*> m_tasks;

    // Index of the current running/scheduled task
    size_t m_currentTaskIndex = 0;

    // Calculates the index of the next schedulable task on the run queue
    size_t _getNextTaskIndex();
};

// Round-robin style scheduler
class RRScheduler {
public:
    RRScheduler() = default;
    ~RRScheduler() = default;

    static RRScheduler& get();
    
    // Allocates the necessary scheduler resources (run-queues, etc.)
    void init();

    // Creates a separate task run
    // queue for the provided cpu core.
    void registerCpuCore(int cpu);

    // Adds a task to the specified cpu core's run queue
    bool addTask(Task* task, int cpu);

    // Adds a task to the next optimal available cpu
    bool addTask(Task* task);

    // Removes a task from the specified
    // cpu core's run queue if it exists.
    bool removeTask(Task* task, int cpu);

    // Removes a task with a specified pid from the
    // specified cpu core's run queue if it exists.
    bool removeTask(pid_t pid, int cpu);

    // Returns the current scheduled task
    // for the specified core's run queue.
    Task* getCurrentTask(int cpu);

    // Returns the next available schedulable
    // task for a specified cpu core.
    Task* peekNextTask(int cpu);

    // Takes the current running task off the run
    // queue of the specified cpu core and schedules
    // the next available task.
    void scheduleNextTask(int cpu);

    // Calculates the next least loaded CPU
    // core to schedule next task(s) on.
    size_t getNextAvailableCpu();

private:
    // Per-core task run queues
    kstl::vector<RoundRobinRunQueue*> m_runQueues;

    // Number of actual usable cpu cores
    size_t m_usableCpuCount;
};

//
// Allocates a task object for a new kernel thread that will
// start its execution at a given function in userspace (DPL=3).
//
Task* createKernelTask(void (*taskEntry)(), int priority = 0);

//
// Allows the current running kernel thread to terminate and switch to the next
// available task without waiting for the next timer interrupt. If no next valid
// task is available, control flow switches back to the kernel swapper task.
//
void exitKernelThread();

#endif
