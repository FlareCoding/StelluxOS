#include "kernel_unit_tests.h"
#include <sched/sched.h>
#include <memory/kmemory.h>
#include <time/ktime.h>
#include <acpi/acpi_controller.h>
#include <kelevate/kelevate.h>
#include <sync.h>

DECLARE_SPINLOCK(mtUnitTestLock);
uint64_t g_mtUnitTestCounter = 0;

void incrementMtUnitTestCounter(void*) {
    acquireSpinlock(&mtUnitTestLock);
    ++g_mtUnitTestCounter;
    releaseSpinlock(&mtUnitTestLock);

    exitKernelThread();
}

DECLARE_UNIT_TEST("Multithreading Test - Kernel Task Creation", mtTaskCreationUnitTest) {
    const size_t iterations = 10000;
    const size_t milestone = 1000;

    Task** taskArray = (Task**)kmalloc(sizeof(Task*) * iterations);

    for (size_t i = 0; i < iterations; i++) {
        Task* task = createKernelTask(incrementMtUnitTestCounter, nullptr);
        ASSERT_TRUE(task, "Failed to allocate a kernel task");

        taskArray[i] = task;

        if ((i + 1) % milestone == 0) {
            kprintf(UNIT_TEST "Allocated %lli tasks\n", i + 1);
        }
    }

    // Free the allocated tasks
    for (size_t i = 0; i < iterations; i++) {
        bool ret = destroyKernelTask(taskArray[i]);
        ASSERT_TRUE(ret, "Failed to destroy and clean up a kernel task");
    }

    // Free the array for holding tasks for this test
    kfree(taskArray);

    return UNIT_TEST_SUCCESS;
}

DECLARE_UNIT_TEST("Multithreading Test - Single Core", mtSingleCoreUnitTest) {
    const size_t taskCount = 1000;
    const int targetCpu = BSP_CPU_ID;
    const int taskExecutionTimeout = 2500;
    auto& sched = Scheduler::get();

    // Allocate a buffer to store the tasks
    Task** taskArray = (Task**)kmalloc(sizeof(Task*) * taskCount);

    // Reset the test counter
    g_mtUnitTestCounter = 0;

    kprintf(UNIT_TEST "Creating %llu test tasks\n", taskCount);

    // Create the tasks
    for (size_t i = 0; i < taskCount; i++) {
        Task* task = createKernelTask(incrementMtUnitTestCounter, nullptr);
        ASSERT_TRUE(task, "Failed to allocate a kernel task");

        taskArray[i] = task;
    }

    // Schedule all the tasks
    sched.preemptDisable();
    for (size_t i = 0; i < taskCount; i++) {
        sched.addTask(taskArray[i], targetCpu);
        // ASSERT_TRUE(ret, "Failed to schedule a task on a single CPU core");
    }
    kprintf(UNIT_TEST "Beginning execution\n");
    sched.preemptEnable();

    // Wait for all tasks to finish
    msleep(taskExecutionTimeout);

    // Check that the counter reached the correct value
    ASSERT_EQ(g_mtUnitTestCounter, taskCount, "Incorrect final value of the test counter after task execution");

    // Destroy the allocated tasks
    for (size_t i = 0; i < taskCount; i++) {
        bool ret = destroyKernelTask(taskArray[i]);
        ASSERT_TRUE(ret, "Failed to destroy and clean up a kernel task");
    }

    // Free the array for holding tasks for this test
    kfree(taskArray);

    return UNIT_TEST_SUCCESS;
}

DECLARE_UNIT_TEST("Multithreading Test - Multi Core (Automatic Load Balancing)", mtMultiCoreUnitTest) {
    const size_t systemCpus = AcpiController::get().getApicTable()->getCpuCount();
    const size_t taskCount = 600 * systemCpus;
    const uint32_t taskExecutionTimeout = 4000;
    auto& sched = Scheduler::get();

    // Allocate a buffer to store the tasks
    Task** taskArray = (Task**)kmalloc(sizeof(Task*) * taskCount);

    // Reset the test counter
    g_mtUnitTestCounter = 0;

    kprintf(UNIT_TEST "Creating %llu test tasks\n", taskCount);

    // Create the tasks
    for (size_t i = 0; i < taskCount; i++) {
        Task* task = createKernelTask(incrementMtUnitTestCounter, nullptr);
        ASSERT_TRUE(task, "Failed to allocate a kernel task");

        taskArray[i] = task;
    }

    // Disable preemption on all cores
    for (size_t cpu = 0; cpu < systemCpus; cpu++) {
        sched.preemptDisable(cpu);
    }

    // Schedule all the tasks
    for (size_t i = 0; i < taskCount; i++) {
        sched.addTask(taskArray[i]);
    }

    kprintf(UNIT_TEST "Beginning execution\n");

    // Re-enable preemption on all cores
    for (size_t cpu = 0; cpu < systemCpus; cpu++) {
        sched.preemptEnable(cpu);
    }

    // Wait for all tasks to finish
    msleep(taskExecutionTimeout);

    // Check that the counter reached the correct value
    ASSERT_EQ(g_mtUnitTestCounter, taskCount, "Incorrect final value of the test counter after asynchronous task execution");

    // Destroy the allocated tasks
    for (size_t i = 0; i < taskCount; i++) {
        bool ret = destroyKernelTask(taskArray[i]);
        ASSERT_TRUE(ret, "Failed to destroy and clean up a kernel task");
    }

    // Free the array for holding tasks for this test
    kfree(taskArray);

    return UNIT_TEST_SUCCESS;
}
