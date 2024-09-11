#include "sched.h"
#include <memory/kmemory.h>
#include <paging/page.h>
#include <gdt/gdt.h>
#include <kelevate/kelevate.h>
#include <sync.h>

#define SCHED_KERNEL_STACK_PAGES    2
#define SCHED_USER_STACK_PAGES      2

#define SCHED_KERNEL_STACK_SIZE     SCHED_KERNEL_STACK_PAGES * PAGE_SIZE
#define SCHED_USER_STACK_SIZE       SCHED_USER_STACK_PAGES * PAGE_SIZE

RRScheduler s_globalRRScheduler;
Task g_kernelSwapperTasks[MAX_CPUS] = {};

DECLARE_SPINLOCK(__sched_lock);
DECLARE_SPINLOCK(__sched_load_balancing_lock);

size_t g_availableTaskPid = 10;
size_t _allocateTaskPid() {
    size_t pid = g_availableTaskPid++;

    // Checking for wrap-around case
    if (g_availableTaskPid == 0) {
        g_availableTaskPid = 10;
    }

    return pid;
}

size_t RoundRobinRunQueue::size() const {
    return m_tasks.size();
}

bool RoundRobinRunQueue::addTask(Task* task) {
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

    // Increment the usable cpu core count
    m_usableCpuCount++;
}

bool RRScheduler::addTask(Task* task, int cpu) {
    if (cpu < 0 || cpu >= MAX_CPUS) {
        // TO-DO: Deal with proper error handling
        asm volatile ("hlt");
        return false;
    }

    acquireSpinlock(&__sched_lock);

    // Assign a cpu to the task
    task->cpu = cpu;

    auto& runQueue = m_runQueues[cpu];
    bool ret = runQueue->addTask(task);

    releaseSpinlock(&__sched_lock);
    return ret;
}

bool RRScheduler::addTask(Task* task) {
    acquireSpinlock(&__sched_load_balancing_lock);

    int cpu = _getNextAvailableCpu();
    bool ret = addTask(task, cpu);

    releaseSpinlock(&__sched_load_balancing_lock);

    return ret;
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

    acquireSpinlock(&__sched_lock);

    auto& runQueue = m_runQueues[cpu];
    bool ret = runQueue->removeTask(pid);

    releaseSpinlock(&__sched_lock);
    return ret;
}

Task* RRScheduler::getCurrentTask(int cpu) {
    if (cpu < 0 || cpu >= MAX_CPUS) {
        // TO-DO: Deal with proper error handling
        asm volatile ("hlt");
        return nullptr;
    }

    acquireSpinlock(&__sched_lock);

    auto& runQueue = m_runQueues[cpu];
    Task* task = runQueue->getCurrentTask();

    releaseSpinlock(&__sched_lock);
    return task;
}

Task* RRScheduler::peekNextTask(int cpu) {
    if (cpu < 0 || cpu >= MAX_CPUS) {
        // TO-DO: Deal with proper error handling
        asm volatile ("hlt");
        return nullptr;
    }

    acquireSpinlock(&__sched_lock);

    auto& runQueue = m_runQueues[cpu];
    Task* task = runQueue->peekNextTask();

    releaseSpinlock(&__sched_lock);
    return task;
}

void RRScheduler::scheduleNextTask(int cpu) {
    if (cpu < 0 || cpu >= MAX_CPUS) {
        // TO-DO: Deal with proper error handling
        asm volatile ("hlt");
        return;
    }

    acquireSpinlock(&__sched_lock);
    
    auto& runQueue = m_runQueues[cpu];
    runQueue->scheduleNextTask();

    releaseSpinlock(&__sched_lock);
}

int RRScheduler::_getNextAvailableCpu() {
    int cpu = 0;
    size_t leastTaskCount = m_runQueues[cpu]->size();

    for (int i = 1; i < (int)m_usableCpuCount; ++i) {
        size_t cpuTaskCount = m_runQueues[i]->size();
        if (cpuTaskCount < leastTaskCount) {
            cpu = i;
            leastTaskCount = cpuTaskCount;
        }
    }

    return cpu;
}

Task* createKernelTask(void (*taskEntry)(), int priority) {
    Task* task = (Task*)kmalloc(sizeof(Task));
    if (!task) {
        return nullptr;
    }

    zeromem(task, sizeof(Task));

    // Initialize the task's process control block
    task->state = ProcessState::READY;
    task->pid = _allocateTaskPid();
    task->priority = priority;

    // Allocate both user and kernel stacks
    void* userStack = zallocPages(SCHED_USER_STACK_PAGES);
    if (!userStack) {
        kfree(task);
        return nullptr;
    }

    void* kernelStack = zallocPages(SCHED_KERNEL_STACK_PAGES);
    if (!kernelStack) {
        kfree(task);
        freePages(userStack, SCHED_USER_STACK_PAGES);
        return nullptr;
    }

    // Initialize the CPU context
    task->context.rsp = (uint64_t)userStack + SCHED_USER_STACK_SIZE; // Point to the top of the stack
    task->context.rbp = task->context.rsp;       // Point to the top of the stack
    task->context.rip = (uint64_t)taskEntry;     // Set instruction pointer to the task function
    task->context.rflags = 0x200;                // Enable interrupts

    // Set up segment registers for user space. These values correspond to the selectors in the GDT.
    task->context.cs = __USER_CS | 0x3;
    task->context.ds = __USER_DS | 0x3;
    task->context.es = task->context.ds;
    task->context.ss = task->context.ds;

    // Save the kernel stack
    task->kernelStack = (uint64_t)kernelStack + SCHED_KERNEL_STACK_SIZE;

    // Save the user stack
    task->userStackTop = task->context.rsp;

    // Setup the task's page table
    task->cr3 = reinterpret_cast<uint64_t>(paging::g_kernelRootPageTable);

    return task;
}

bool destroyKernelTask(Task* task) {
    if (!task) {
        return false;
    }

    // Destroy the stacks
    freePages((void*)(task->kernelStack - SCHED_KERNEL_STACK_SIZE), SCHED_KERNEL_STACK_PAGES);
    freePages((void*)(task->userStackTop - SCHED_USER_STACK_SIZE), SCHED_USER_STACK_PAGES);

    // Free the actual task structure
    kfree(task);

    return true;
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
    // In the event when because of some scheduling circumstances we
    // end up with the next task being the same one we are trying to
    // terminate, switch to the default kernel swapper task.
    //
    if (currentTask == nextTask) {
        nextTask = &g_kernelSwapperTasks[cpu];

        // TO-DO: Assert that the current task is also not the swapper task
    }

    // Remove the current task from the run queue
    sched.removeTask(currentTask->pid, cpu);

    // Switch to the next available task if possible
    sched.scheduleNextTask(cpu);

    // This will end up calling an assembly routine that results in an 'iretq'
    exitAndSwitchCurrentContext(cpu, nextTask, &regs);
}

