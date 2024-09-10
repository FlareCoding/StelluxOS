#include "kernel_unit_tests.h"
#include <sched/sched.h>
#include <memory/kmemory.h>
#include <sync.h>

DECLARE_SPINLOCK(mtUnitTestLock);
uint64_t g_mtUnitTestCounter = 0;

void incrementMtUnitTestCounter() {
    acquireSpinlock(&mtUnitTestLock);
    ++g_mtUnitTestCounter;
    releaseSpinlock(&mtUnitTestLock);

    exitKernelThread();
}

// DECLARE_UNIT_TEST("Multithreading Test - Kernel Task Creation", mtTaskCreationUnitTest) {
//     const size_t iterations = 10000;
//     const size_t milestone = 1000;

//     for (size_t i = 1; i <= iterations; i++) {
//         // Task* task = createKernelTask(incrementMtUnitTestCounter);
//         Task* task = (Task*)kmalloc(sizeof(Task));
//         if (!task) {
//             kuPrint(UNIT_TEST "Failed to create kernel task on iteration %lli\n", i);
//             return UNIT_TEST_FAILURE;
//         }
//         // zeromem(task, sizeof(Task));

//         // Allocate both user and kernel stacks
//         void* userStack = zallocPages(2);
//         void* kernelStack = zallocPages(2);
//         if (!userStack || !kernelStack) {
//             kuPrint(UNIT_TEST "Failed to create user or kernel stack on iteration %lli\n", i);
//             return UNIT_TEST_FAILURE;
//         }

//         ASSERT_TRUE(task, "Failed to create kernel task");

//         if (i % milestone == 0) {
//             kuPrint(UNIT_TEST "Allocated %lli tasks\n", i);
//         }
//     }

//     return UNIT_TEST_SUCCESS;
// }
