#include "sched.h"
#include <memory/kmemory.h>
#include <paging/page.h>
#include <gdt/gdt.h>
#include <arch/x86/apic.h>
#include <sync.h>

#define SCHED_KERNEL_STACK_PAGES    2
#define SCHED_USER_STACK_PAGES      2

#define SCHED_KERNEL_STACK_SIZE     SCHED_KERNEL_STACK_PAGES * PAGE_SIZE
#define SCHED_USER_STACK_SIZE       SCHED_USER_STACK_PAGES * PAGE_SIZE

Scheduler g_primaryScheduler;
Task g_kernelSwapperTasks[MAX_CPUS] = {};

DECLARE_SPINLOCK(__sched_pid_alloc_lock);
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

SchedRunQueue::SchedRunQueue(Task* idleTask) {
    m_tasks.pushBack(idleTask);
}

void SchedRunQueue::addTask(Task* task) {
    acquireSpinlock(&m_lock);
    m_tasks.pushBack(task);
    releaseSpinlock(&m_lock);
}
    
void SchedRunQueue::removeTask(Task* task) {
    acquireSpinlock(&m_lock);
    
    size_t idx = m_tasks.find(task);
    if (idx != kstl::npos) {
        m_tasks.erase(idx);
        _incrementTaskIndex();
    }

    releaseSpinlock(&m_lock);
}

Task* SchedRunQueue::scheduleNextTask() {
    Task* nextTask = nullptr;

    acquireSpinlock(&m_lock);
    
    size_t taskCount = m_tasks.size();
    if (taskCount == 1) {
        nextTask = m_tasks[0]; // idle task
    } else if (taskCount == 2) {
        nextTask = m_tasks[1]; // first real task
    } else {
        _incrementTaskIndex();
        nextTask = m_tasks[m_currentTaskIdx];
    }

    releaseSpinlock(&m_lock);
    return nextTask;
}

size_t SchedRunQueue::size() {
    size_t sz = 0;
    acquireSpinlock(&m_lock);

    sz = m_tasks.size();
    releaseSpinlock(&m_lock);

    return sz;
}

void SchedRunQueue::_incrementTaskIndex() {
    const size_t taskCount = m_tasks.size();

    // Handle the wrap-around case
    if (++m_currentTaskIdx >= taskCount) {
        m_currentTaskIdx = 0;
    }

    // If any real task is present, prioritze
    // it over the idle swapper task.
    if (m_currentTaskIdx == 0 && taskCount > 1) {
        m_currentTaskIdx = 1;
    }
}

Scheduler& Scheduler::get() {
    return g_primaryScheduler;
}

void Scheduler::init() {
    registerCpuRunQueue(BSP_CPU_ID);
}

void Scheduler::registerCpuRunQueue(int cpu) {
    auto runQueue = kstl::SharedPtr<SchedRunQueue>(
        new SchedRunQueue(&g_kernelSwapperTasks[cpu])
    );
    
    m_runQueues.pushBack(runQueue);
}

void Scheduler::addTask(Task* task, int cpu) {
    if (cpu == -1) {
        preemptDisable();
        cpu = static_cast<int>(_loadBalance());
        preemptEnable();
    }

    // Mask the timer interrupts while adding the task to the queue
    preemptDisable(cpu);

    // Prepare the task
    task->cpu = cpu;
    task->state = ProcessState::READY;

    // Atomically add the task to the run-queue of the target processor
    m_runQueues[cpu]->addTask(task);

    // Unmask the timer interrupt and continue as usual
    preemptEnable(cpu);
}

void Scheduler::removeTask(Task* task) {
    int cpu = task->cpu;

    // Mask the timer interrupts while removing the task from the queue
    preemptDisable(cpu);

    // Atomically add the task to the run-queue of the target processor
    m_runQueues[cpu]->removeTask(task);

    // Unmask the timer interrupt and continue as usual
    preemptEnable(cpu);
}

void Scheduler::__schedule(PtRegs* irqFrame) {
    int cpu = current->cpu;
    Task* nextTask = m_runQueues[cpu]->scheduleNextTask();
    if (nextTask && nextTask != current) {
        switchContextInIrq(cpu, cpu, current, nextTask, irqFrame);
    }
}

void Scheduler::schedule() {
    asm volatile ("int $47");
}

void Scheduler::preemptDisable(int cpu) {
    if (cpu == -1) {
        cpu = current->cpu;
    }

    Apic::getLocalApicForCpu(cpu)->maskTimerIrq();
}

void Scheduler::preemptEnable(int cpu) {
    if (cpu == -1) {
        cpu = current->cpu;
    }

    Apic::getLocalApicForCpu(cpu)->unmaskTimerIrq();
}

size_t Scheduler::_loadBalance() {
    size_t cpu = 0;
    size_t smallestTaskCount = m_runQueues[cpu]->size();

    for (size_t i = 1; i < m_runQueues.size(); i++) {
        size_t cnt = m_runQueues[i]->size();
        if (cnt < smallestTaskCount) {
            cpu = i;
            smallestTaskCount = cnt;
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
    auto& sched = Scheduler::get();
    
    // Remove the current task from the run queue
    sched.removeTask(current);

    // Trigger a schedule sequence which will switch
    // the context to the next available task.
    sched.schedule();
}

