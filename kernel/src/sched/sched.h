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

    /**
     * @brief Returns the singleton instance of the Scheduler.
     * 
     * @return Scheduler& Reference to the Scheduler instance.
     */
    static Scheduler& get();

    /**
     * @brief Initializes the Scheduler system.
     * 
     * This function sets up necessary data structures and prepares 
     * the Scheduler to manage tasks across multiple CPU cores.
     */
    void init();

    /**
     * @brief Registers a CPU core for task scheduling.
     * 
     * This function marks a given CPU core as eligible for task scheduling. 
     * This ensures that tasks can be assigned and managed on this core.
     * 
     * @param cpu The CPU core number to register.
     */
    void registerCoreForScheduling(int cpu);

    /**
     * @brief Retrieves the currently running task on the specified CPU.
     * 
     * This function returns a pointer to the task that is currently 
     * being executed on the specified CPU core.
     * 
     * @param cpu The CPU core number to query.
     * @return Task* Pointer to the currently running task on the specified CPU.
     */
    Task* getCurrentTask(int cpu);

    /**
     * @brief Peeks at the next task in the queue for the specified CPU.
     * 
     * This function returns a pointer to the next task that is 
     * scheduled to run on the specified CPU core without actually 
     * switching to it. This allows you to inspect the task that 
     * will be executed next.
     * 
     * @param cpu The CPU core number to query.
     * @return Task* Pointer to the next task in the queue for the specified CPU.
     */
    Task* peekNextTask(int cpu);

    /**
     * @brief Schedules the next task to run on the specified CPU.
     * 
     * This function performs the task switch for the given CPU core. 
     * It moves the next task in the queue to the running state and 
     * ensures that the previous task is stopped or moved to the 
     * appropriate state (e.g., waiting).
     * 
     * @param cpu The CPU core number to schedule the next task on.
     */
    void scheduleNextTask(int cpu);

    /**
     * @brief Adds a task to a specific CPU core's run queue.
     * 
     * This function assigns a task to the specified CPU's scheduling 
     * queue. The task will eventually be scheduled and executed by 
     * the Scheduler on that core.
     * 
     * @param task Pointer to the task to be scheduled.
     * @param cpu The CPU core number to add the task to.
     */
    void addTaskToCpu(Task* task, int cpu);

    /**
     * @brief Adds a task to the least loaded CPU core.
     * 
     * This function examines the load on all available CPU cores and 
     * assigns the task to the core that has the fewest running tasks 
     * or lowest load. It provides a simple form of load balancing 
     * to distribute tasks efficiently.
     * 
     * @param task Pointer to the task to be scheduled.
     */
    void addTask(Task* task);

    /**
     * @brief Removes a task from a specific CPU core's run queue.
     * 
     * This function removes the task with the given process ID (PID) 
     * from the specified CPU's scheduling queue. This may occur if 
     * the task has completed execution or has been explicitly stopped.
     * 
     * @param pid The process ID of the task to be removed.
     * @param cpu The CPU core number from which to remove the task.
     */
    void removeTaskFromCpu(int pid, int cpu);

    /**
     * @brief Voluntarily yields the processor from the current task.
     * 
     * This function allows the current running task to voluntarily 
     * yield its execution to allow other tasks to run. It triggers 
     * the scheduler to select another task to execute on the current 
     * CPU core. This is typically used when a task has no immediate 
     * work to do and wishes to give up the CPU.
     */
    void yieldTask();

private:
    // List of run queues for each CPU core, holding tasks to be scheduled on that core
    kstl::vector<kstl::SharedPtr<SchedulerRunQueue>> m_runQueues;

    /**
     * @brief Finds the least loaded CPU core.
     * 
     * This function inspects the run queues of all registered CPU cores 
     * and returns the core number of the least loaded one. This is 
     * typically used for load balancing when adding new tasks.
     * 
     * @return int The CPU core number with the least load.
     */
    int _findLeastLoadedCpu();

    /**
     * @brief Retrieves the size of the run queue for the specified CPU.
     * 
     * This function returns the number of tasks currently in the scheduling 
     * queue for the specified CPU core, providing insight into the load on that core.
     * 
     * @param cpu The CPU core number to query.
     * @return size_t The number of tasks in the run queue for the specified CPU.
     */
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
