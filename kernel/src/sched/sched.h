#ifndef SCHED_H
#define SCHED_H
#include <core/kvector.h>
#include <arch/x86/per_cpu_data.h>
#include <process/process.h>
#include <sync.h>

using Task = PCB;

EXTERN_C Task g_kernelSwapperTasks[MAX_CPUS];

class SchedRunQueue {
public:
    SchedRunQueue(Task* idleTask);
    ~SchedRunQueue() = default;

    void addTask(Task* task);
    
    void removeTask(Task* task);

    Task* scheduleNextTask();

    size_t size();

private:
    void _incrementTaskIndex();

private:
    DECLARE_SPINLOCK(m_lock);
    
    kstl::vector<Task*> m_tasks;
    size_t m_currentTaskIdx = 0;
};

class Scheduler {
public:
    Scheduler() = default;
    ~Scheduler() = default;

    static Scheduler& get();

    void init();
    void registerCpuRunQueue(int cpu);

    void addTask(Task* task, int cpu = -1);
    void removeTask(Task* task);

    // Called from the IRQ interrupt context. Picks the
    // next task to run and switches the context into it.
    void __schedule(PtRegs* irqFrame);

    // Forces a new task to get scheduled and triggers a
    // context switch without the need for a timer tick.
    void schedule();

    // Masks timer tick-based interrupts
    void preemptDisable();

    // Unmasks timer tick-based interrupts
    void preemptEnable();

private:
    kstl::vector<kstl::SharedPtr<SchedRunQueue>> m_runQueues;

    // Returns the cpu with the smallest number of queued tasks
    size_t _loadBalance();
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
