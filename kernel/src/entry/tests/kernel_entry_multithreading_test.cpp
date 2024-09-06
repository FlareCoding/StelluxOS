#include "kernel_entry_tests.h"
#include <core/kprint.h>
#include <core/kstring.h>
#include <memory/kmemory.h>
#include <syscall/syscalls.h>
#include <kelevate/kelevate.h>
#include <sched/sched.h>

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

void ke_test_multithreading() {
    auto& sched = RRScheduler::get();

    // Create some test tasks and add them to the scheduler
    Task* task1 = createKernelTask(simpleFunctionElevKprint);
    Task* task2 = createKernelTask(simpleFunctionSyscallPrint);
    Task* task3 = createKernelTask(simpleFunctionKuprint);
    Task* task4 = createKernelTask(sayHelloTask);

    sched.addTask(task1, BSP_CPU_ID);
    sched.addTask(task2, BSP_CPU_ID);
    sched.addTask(task3, BSP_CPU_ID);
    sched.addTask(task4, BSP_CPU_ID);
}
