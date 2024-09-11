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

Scheduler g_primaryScheduler;
Task g_kernelSwapperTasks[MAX_CPUS] = {};

DECLARE_SPINLOCK(__sched_pid_alloc_lock);
DECLARE_SPINLOCK(__sched_load_balancing_lock);

size_t g_availableTaskPid = 10;
size_t _allocateTaskPid() {
    acquireSpinlock(&__sched_pid_alloc_lock);
    size_t pid = g_availableTaskPid++;

    // Checking for wrap-around case
    if (g_availableTaskPid == 0) {
        g_availableTaskPid = 10;
    }

    releaseSpinlock(&__sched_pid_alloc_lock);
    return pid;
}

Scheduler& Scheduler::get() {
    return g_primaryScheduler;
}

void Scheduler::init() {
    registerCoreForScheduling(BSP_CPU_ID);
}

void Scheduler::registerCoreForScheduling(int cpu) {
    // Create a new run queue for the core with a swapper task
    auto runQueue = kstl::SharedPtr<SchedulerRunQueue>(new SchedulerRunQueue());
    runQueue->tasks.pushBack(&g_kernelSwapperTasks[cpu]);
    runQueue->currentTaskIndex = 0;

    // Add the run queue to the scheduler
    m_runQueues.pushBack(runQueue);
}

Task* Scheduler::getCurrentTask(int cpu) {
    Task* task;
    acquireSpinlock(&m_runQueues[cpu]->lock);

    size_t idx = m_runQueues[cpu]->currentTaskIndex;
    task = m_runQueues[cpu]->tasks[idx];

    releaseSpinlock(&m_runQueues[cpu]->lock);
    return task;
}

Task* Scheduler::peekNextTask(int cpu) {
    Task* task;
    acquireSpinlock(&m_runQueues[cpu]->lock);

    size_t idx = m_runQueues[cpu]->currentTaskIndex + 1;
    if (idx == m_runQueues[cpu]->tasks.size()) {
        idx = 0;
    }

    task = m_runQueues[cpu]->tasks[idx];

    releaseSpinlock(&m_runQueues[cpu]->lock);
    return task;
}

void Scheduler::scheduleNextTask(int cpu) {
    acquireSpinlock(&m_runQueues[cpu]->lock);

    size_t curIdx = m_runQueues[cpu]->currentTaskIndex;
    size_t nextIdx = m_runQueues[cpu]->currentTaskIndex + 1;

    if (nextIdx == m_runQueues[cpu]->tasks.size()) {
        nextIdx = 0;
    }

    if (curIdx != nextIdx) {
        m_runQueues[cpu]->tasks[curIdx]->state = ProcessState::READY;
        m_runQueues[cpu]->tasks[nextIdx]->state = ProcessState::RUNNING;

        // Update index of current running task
        m_runQueues[cpu]->currentTaskIndex = nextIdx;
    }

    releaseSpinlock(&m_runQueues[cpu]->lock);
}

void Scheduler::addTaskToCpu(Task* task, int cpu) {
    acquireSpinlock(&m_runQueues[cpu]->lock);

    task->cpu = cpu;
    m_runQueues[cpu]->tasks.pushBack(task);

    releaseSpinlock(&m_runQueues[cpu]->lock);
}

void Scheduler::addTask(Task* task) {
    size_t targetCpu = _findLeastLoadedCpu();
    addTaskToCpu(task, targetCpu);
}

void Scheduler::removeTaskFromCpu(int pid, int cpu) {
    acquireSpinlock(&m_runQueues[cpu]->lock);

    for (size_t i = 0; i < m_runQueues[cpu]->tasks.size(); i++) {
        if (m_runQueues[cpu]->tasks[i]->pid == pid) {
            m_runQueues[cpu]->tasks.erase(i);
            m_runQueues[cpu]->currentTaskIndex = 0;
            break;
        }
    }

    releaseSpinlock(&m_runQueues[cpu]->lock);
}

int Scheduler::_findLeastLoadedCpu() {
    int cpu = 0;
    size_t leastTasksFound = static_cast<size_t>(-1);

    acquireSpinlock(&__sched_load_balancing_lock);

    for (size_t i = 0; i < m_runQueues.size(); i++) {
        size_t cpuTasks = _getRunQueueSize(i);
        if (cpuTasks < leastTasksFound) {
            leastTasksFound = cpuTasks;
            cpu = static_cast<int>(i);
        }
    }

    releaseSpinlock(&__sched_load_balancing_lock);
    return cpu;
}

size_t Scheduler::_getRunQueueSize(size_t cpu) {
    size_t ret = 0;

    acquireSpinlock(&m_runQueues[cpu]->lock);
    ret = m_runQueues[cpu]->tasks.size();

    releaseSpinlock(&m_runQueues[cpu]->lock);
    return ret;
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
    auto& sched = Scheduler::get();

    // Elevate for the context switch and to disable the interrupts
    __kelevate();
    disableInterrupts();

    int cpu = current->cpu;

    // Get the current running task
    PCB* currentTask = sched.getCurrentTask(cpu);
    
    // Remove the current task from the run queue
    sched.removeTaskFromCpu(currentTask->pid, cpu);

    // Switch to the next available task if possible
    sched.scheduleNextTask(cpu);

    // Get the new running task
    PCB* newTask = sched.getCurrentTask(cpu);

    // This will end up calling an assembly routine that results in an 'iretq'
    exitAndSwitchCurrentContext(cpu, newTask, &regs);
}

