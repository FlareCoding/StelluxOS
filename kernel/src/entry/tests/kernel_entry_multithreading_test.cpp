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
    __kelevate();
    while(1) {
        int result = fibb(36);
        (void)result;

        const char* msg = "simpleFunctionSyscallPrint> Calculated fibb(36)! Ignoring result...\n";
        __syscall(SYSCALL_SYS_WRITE, 0, (uint64_t)msg, strlen(msg), 0, 0, 0);
    }
}

void simpleFunctionKuprint() {
    __kelevate();
    while(1) {
        int result = fibb(34);
        kuPrint("simpleFunctionKuprint> fibb(34): %i\n", result);
    }
}

PCB createKernelTask(task_function_t taskFunction, uint64_t pid) {
    PCB newTask;
    zeromem(&newTask, sizeof(PCB));

    // Initialize the PCB
    memset(&newTask, 0, sizeof(PCB));
    newTask.state = ProcessState::READY;
    newTask.pid = pid;
    newTask.priority = 0;

    // Allocate both user and kernel stacks
    void* stack = zallocPage();
    void* kernelStack = zallocPage();

    // Initialize the CPU context
    newTask.context.rsp = (uint64_t)stack + PAGE_SIZE;  // Point to the top of the stack
    newTask.context.rbp = newTask.context.rsp;          // Point to the top of the stack
    newTask.context.rip = (uint64_t)taskFunction;       // Set instruction pointer to the task function
    newTask.context.rflags = 0x200;                     // Enable interrupts

    // Set up segment registers for user space. These values correspond to the selectors in the GDT.
    newTask.context.cs = __USER_CS | 0x3;
    newTask.context.ds = __USER_DS | 0x3;
    newTask.context.es = newTask.context.ds;
    newTask.context.ss = newTask.context.ds;

    // Save the kernel stack
    newTask.kernelStack = (uint64_t)kernelStack + PAGE_SIZE;

    // Setup the task's page table
    newTask.cr3 = reinterpret_cast<uint64_t>(paging::g_kernelRootPageTable);

    return newTask;
}

void ke_test_multithreading() {
    auto& sched = RoundRobinScheduler::get();

    // Create some tasks and add them to the scheduler
    PCB task1, task2, task3;

    task1 = createKernelTask(simpleFunctionElevKprint, 2);
    task2 = createKernelTask(simpleFunctionSyscallPrint, 3);
    task3 = createKernelTask(simpleFunctionKuprint, 4);

    sched.addTask(task1);
    sched.addTask(task2);
    sched.addTask(task3);
}
