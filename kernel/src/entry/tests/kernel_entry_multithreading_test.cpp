#include "kernel_entry_tests.h"
#include <core/kprint.h>
#include <core/kstring.h>
#include <memory/kmemory.h>
#include <paging/page.h>
#include <syscall/syscalls.h>
#include <kelevate/kelevate.h>
#include <process/process.h>
#include <gdt/gdt.h>
#include <sched/sched.h>

// Function prototype for the task function
typedef void (*task_function_t)();

int fibb(int n) {
    if (n == 0 || n == 1) {
        return n;
    }

    return fibb(n - 1) + fibb(n - 2);
}

// use recursive function to exercise context switch (fibb)
void simpleFunctionElevKprint() {
    __kelevate();
    while(1) {
        int result = fibb(32);
        kprint("simpleFunctionElevKprint>  fibb(32): %i\n", result);
    }
}

void simpleFunctionSyscallPrint() {
    while(1) {
        int result = fibb(36);
        (void)result;

        const char* msg = "simpleFunctionSyscallPrint> Calculated fibb(36)! Ignoring result...\n";
        __syscall(SYSCALL_SYS_WRITE, 0, (uint64_t)msg, strlen(msg), 0, 0, 0);
    }
}

void simpleFunctionKuprint() {
    for (int i = 0; i < 5; i++) {
        int result = fibb(34);
        kuPrint("simpleFunctionKuprint> fibb(34): %i\n", result);
    }

    exitKernelThread();
}

void sayHelloTask() {
    kuPrint("Hello!\n");
    exitKernelThread();
}

Task* createKernelTask(task_function_t taskFunction, uint64_t pid) {
    Task* task = (Task*)kmalloc(sizeof(Task));
    zeromem(task, sizeof(Task));

    // Initialize the task's process control block
    task->state = ProcessState::READY;
    task->pid = pid;
    task->priority = 0;

    // Allocate both user and kernel stacks
    void* userStack = zallocPages(2);
    void* kernelStack = zallocPages(2);

    // Initialize the CPU context
    task->context.rsp = (uint64_t)userStack + PAGE_SIZE; // Point to the top of the stack
    task->context.rbp = task->context.rsp;               // Point to the top of the stack
    task->context.rip = (uint64_t)taskFunction;          // Set instruction pointer to the task function
    task->context.rflags = 0x200;                        // Enable interrupts

    // Set up segment registers for user space. These values correspond to the selectors in the GDT.
    task->context.cs = __USER_CS | 0x3;
    task->context.ds = __USER_DS | 0x3;
    task->context.es = task->context.ds;
    task->context.ss = task->context.ds;

    // Save the kernel stack
    task->kernelStack = (uint64_t)kernelStack + PAGE_SIZE;

    // Setup the task's page table
    task->cr3 = reinterpret_cast<uint64_t>(paging::g_kernelRootPageTable);

    return task;
}

void ke_test_multithreading() {
    auto& sched = RRScheduler::get();

    // Create some test tasks and add them to the scheduler
    Task* task1 = createKernelTask(simpleFunctionElevKprint, 2);
    Task* task2 = createKernelTask(simpleFunctionSyscallPrint, 3);
    Task* task3 = createKernelTask(simpleFunctionKuprint, 4);
    Task* task4 = createKernelTask(sayHelloTask, 5);

    sched.addTask(task1, BSP_CPU_ID);
    sched.addTask(task2, BSP_CPU_ID);
    sched.addTask(task3, BSP_CPU_ID);
    sched.addTask(task4, BSP_CPU_ID);
}
