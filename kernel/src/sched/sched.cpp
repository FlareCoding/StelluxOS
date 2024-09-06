#include "sched.h"
#include <memory/kmemory.h>
#include <kelevate/kelevate.h>

RRScheduler s_globalRRScheduler;

Task g_kernelSwapperTasks[MAX_CPUS] = {};

size_t RoundRobinRunQueue::size() const {
    return m_tasks.size();
}

bool RoundRobinRunQueue::addTask(Task* task) {
    if (m_tasks.size() == MAX_QUEUED_PROCESSES) {
        // The queue limit has been reached
        return false;
    }

    // Add the task to the run queue
    m_tasks.pushBack(task);
    return true;
}

bool RoundRobinRunQueue::removeTask(pid_t pid) {
    // Kernel swapper tasks cannot be removed
    if (pid == 0) {
        return false;
    }

    // TO-DO: Assert that the size of the run queue is more than 1

    for (size_t i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks[i]->pid == pid) {
            m_tasks.erase(i);

            // For now the easiest way to deal with a task
            // getting removed is to just throw the last
            // available task onto the run queue.
            m_currentTaskIndex = m_tasks.size() - 1;
            return true;
        }
    }

    return false;
}

Task* RoundRobinRunQueue::getCurrentTask() {
    // TO-DO: Assert that the size of the run queue is at least 1
    return m_tasks[m_currentTaskIndex];
}

Task* RoundRobinRunQueue::peekNextTask() {
    // TO-DO: Assert that the size of the run queue is at least 1

    size_t nextTaskIndex = _getNextTaskIndex();
    return m_tasks[nextTaskIndex];
}

void RoundRobinRunQueue::scheduleNextTask() {
    size_t nextTaskIndex = _getNextTaskIndex();
    if (m_currentTaskIndex == nextTaskIndex) {
        // No new schedulable task discovered
        return;
    }

    Task* currentTask = getCurrentTask();
    Task* nextTask = m_tasks[nextTaskIndex];

    // Update previous/current and next task's states
    currentTask->state = ProcessState::READY;
    nextTask->state = ProcessState::RUNNING;

    // Update the tracking task index
    m_currentTaskIndex = nextTaskIndex;
}

size_t RoundRobinRunQueue::_getNextTaskIndex() {
    size_t taskCount = m_tasks.size();
    size_t nextIndex = m_currentTaskIndex;
    
    // Kernel swapper task is the only valid task on the run queue
    if (taskCount == 1) {
        return 0;
    }
    
    // Always stay on the first valid task and ignore the kernel swapper task
    if (taskCount == 2) {
        return 1;
    }

    // Wwhen there are more than 2 tasks, we want to skip index 0
    // since we don't want to waste time in the kernel swapper task.
    if (taskCount > 2) {
        // Advance to the next index wrapping around if needed
        nextIndex = (m_currentTaskIndex + 1) % taskCount;
        
        // If the new index is 0, advance one more to skip it
        if (nextIndex == 0) {
            nextIndex = 1;
        }
    }

    return nextIndex;
}

RRScheduler& RRScheduler::get() {
    return s_globalRRScheduler;
}

void RRScheduler::init() {
    // Allocate enough run queues for the
    // maximum number of supported cores.
    m_runQueues.reserve(MAX_CPUS);

    // Register the run queue for the bootstrapping core
    registerCpuCore(BSP_CPU_ID);
}

void RRScheduler::registerCpuCore(int cpu) {
    if (cpu < 0 || cpu >= MAX_CPUS) {
        // TO-DO: Deal with proper error handling
        asm volatile ("hlt");
        return;
    }

    // The very first task for each run queue must be the kernel swapper
    // task in case the run queue runs out of application tasks to schedule.
    RoundRobinRunQueue* runQueue = new RoundRobinRunQueue();
    m_runQueues[cpu] = runQueue;

    if (runQueue->size() == 0) {
        runQueue->addTask(&g_kernelSwapperTasks[cpu]);
    }
}

bool RRScheduler::addTask(Task* task, int cpu) {
    if (cpu < 0 || cpu >= MAX_CPUS) {
        // TO-DO: Deal with proper error handling
        asm volatile ("hlt");
        return false;
    }

    auto& runQueue = m_runQueues[cpu];
    return runQueue->addTask(task);
}

bool RRScheduler::removeTask(Task* task, int cpu) {
    return removeTask(task->pid, cpu);
}

bool RRScheduler::removeTask(pid_t pid, int cpu) {
    if (cpu < 0 || cpu >= MAX_CPUS) {
        // TO-DO: Deal with proper error handling
        asm volatile ("hlt");
        return false;
    }

    auto& runQueue = m_runQueues[cpu];
    return runQueue->removeTask(pid);
}

Task* RRScheduler::getCurrentTask(int cpu) {
    if (cpu < 0 || cpu >= MAX_CPUS) {
        // TO-DO: Deal with proper error handling
        asm volatile ("hlt");
        return nullptr;
    }

    auto& runQueue = m_runQueues[cpu];
    return runQueue->getCurrentTask(); 
}

Task* RRScheduler::peekNextTask(int cpu) {
    if (cpu < 0 || cpu >= MAX_CPUS) {
        // TO-DO: Deal with proper error handling
        asm volatile ("hlt");
        return nullptr;
    }

    auto& runQueue = m_runQueues[cpu];
    return runQueue->peekNextTask();
}

void RRScheduler::scheduleNextTask(int cpu) {
    if (cpu < 0 || cpu >= MAX_CPUS) {
        // TO-DO: Deal with proper error handling
        asm volatile ("hlt");
        return;
    }
    
    auto& runQueue = m_runQueues[cpu];
    runQueue->scheduleNextTask();
}

void exitKernelThread() {
    // Construct a fake PtRegs structure to switch to a new context
    PtRegs regs;
    auto& sched = RRScheduler::get();
    
    // Elevate for the context switch and to disable the interrupts
    __kelevate();
    disableInterrupts();

    int cpu = current->cpu;

    PCB* currentTask = sched.getCurrentTask(cpu);
    PCB* nextTask = sched.peekNextTask(cpu);
    
    //
    // TO-DO: Properly assert that nextTask is
    //        not equal to the currentTask.
    //
    if (currentTask == nextTask) {
        return;
    }

    // Remove the current task from the run queue
    sched.removeTask(currentTask->pid, cpu);

    // Switch to the next available task if possible
    sched.scheduleNextTask(cpu);

    // This will end up calling an assembly routine that results in an 'iretq'
    exitAndSwitchCurrentContext(nextTask, &regs);
}

