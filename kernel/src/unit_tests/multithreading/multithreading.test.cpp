#include <unit_tests/unit_tests.h>
#include <memory/memory.h>
#include <memory/vmm.h>
#include <sched/sched.h>
#include <time/time.h>

using namespace sched;

// Shared data for tests
int global_counter = 0;
int global_mutex_counter = 0;

DECLARE_GLOBAL_OBJECT(spinlock, g_multithreading_test_counter_lock);
DECLARE_GLOBAL_OBJECT(mutex, g_multithreading_test_mutex);

// A simple task function that increments a shared counter N times
void increment_task(void* data) {
    int increments = *(int*)data;

    for (int i = 0; i < increments; i++) {
        // Lock and increment
        g_multithreading_test_counter_lock.lock();
        global_counter++;
        g_multithreading_test_counter_lock.unlock();

        // Yield to allow other tasks to run
        yield();
    }

    exit_thread();
}

// A task that immediately exits
void exit_immediately_task(void* data) {
    __unused data;
    exit_thread();
}

// A task that tests mutex usage
void mutex_increment_task(void* data) {
    int increments = *(int*)data;
    for (int i = 0; i < increments; i++) {
        {
            mutex_guard guard(g_multithreading_test_mutex);
            global_mutex_counter++;
        }
        yield();
    }
    exit_thread();
}

// Test creating a single task and letting it run and exit
DECLARE_UNIT_TEST("multithread single task run and exit", test_single_task_run) {
    global_counter = 0;

    int increments = 10;
    task_control_block* task = create_priv_kernel_task(increment_task, &increments);
    ASSERT_TRUE(task != nullptr, "create_priv_kernel_task should return a valid task");

    // Schedule the task on a random CPU
    sched::scheduler::get().add_task(task);

    // Run the scheduler by calling yield until the task is done
    // Assuming that after 20 yields, the task will finish increments
    for (int i = 0; i < 20; i++) {
        yield();
    }

    // Make sure all the tasks on all cpus fully finish within a 1 second interval
    sleep(1);

    // After the task finishes (calls exit_thread), it should not run again
    // Check the counter
    ASSERT_EQ(global_counter, increments, "The global counter should match increments count");

    return UNIT_TEST_SUCCESS;
}

// Test multiple tasks running concurrently, each incrementing the counter
DECLARE_UNIT_TEST("multithread multiple tasks", test_multiple_tasks) {
    global_counter = 0;
    const int increments_per_task = 5;
    const int num_tasks = 4;

    task_control_block* tasks[num_tasks];
    for (int i = 0; i < num_tasks; i++) {
        tasks[i] = create_priv_kernel_task(increment_task, (void*)&increments_per_task);
        ASSERT_TRUE(tasks[i] != nullptr, "Task creation should succeed");
        sched::scheduler::get().add_task(tasks[i]);
    }

    // Run for enough yields to ensure all tasks finish
    // Each task does increments_per_task increments, total increments = num_tasks * increments_per_task
    for (int i = 0; i < num_tasks * increments_per_task * 2; i++) {
        yield();
    }

    // Make sure all the tasks on all cpus fully finish within a 1 second interval
    sleep(1);

    ASSERT_EQ(global_counter, num_tasks * increments_per_task, "All tasks should have incremented the counter collectively");

    return UNIT_TEST_SUCCESS;
}

// Test scheduling tasks onto specific CPUs
DECLARE_UNIT_TEST("multithread per-CPU tasks", test_per_cpu_tasks) {
    global_counter = 0;
    const int increments_per_task = 3;
    const int num_tasks = 2;
    // You mentioned at most 8 CPUs, so we assume cpu IDs from 0 to 7.

    for (int cpu_id = 0; cpu_id < num_tasks; cpu_id++) {
        int* data = (int*)zmalloc(sizeof(int));
        *data = increments_per_task;
        task_control_block* task = create_priv_kernel_task(increment_task, data);
        ASSERT_TRUE(task != nullptr, "Task creation should succeed");
        sched::scheduler::get().add_task(task, cpu_id);
    }

    // Run yields enough times
    for (int i = 0; i < num_tasks * increments_per_task * 2; i++) {
        yield();
    }

    // Make sure all the tasks on all cpus fully finish within a 1 second interval
    sleep(1);

    ASSERT_EQ(global_counter, num_tasks * increments_per_task, "Tasks scheduled on different CPUs should run and increment the counter");

    return UNIT_TEST_SUCCESS;
}

// Test that a task that exits immediately doesn't affect the system
DECLARE_UNIT_TEST("multithread exit immediate task", test_exit_immediate) {
    task_control_block* task = create_priv_kernel_task(exit_immediately_task, nullptr);
    ASSERT_TRUE(task != nullptr, "Should create task");

    sched::scheduler::get().add_task(task, 0);
    // Just yield a few times, the task should exit immediately
    yield();
    yield();

    // No global state to check here, just ensure no crash
    return UNIT_TEST_SUCCESS;
}

// Test destroying a task that was never run (just to confirm resource cleanup)
DECLARE_UNIT_TEST("multithread destroy task before run", test_destroy_before_run) {
    task_control_block* task = create_priv_kernel_task(exit_immediately_task, nullptr);
    ASSERT_TRUE(task != nullptr, "Should create task");

    // Destroy the task without scheduling it
    bool success = destroy_task(task);
    ASSERT_TRUE(success, "Destroying the task before run should succeed");

    // No crash, no double free expected
    return UNIT_TEST_SUCCESS;
}

// Test using a mutex with multiple tasks incrementing a counter
DECLARE_UNIT_TEST("multithread mutex test", test_mutex_usage) {
    global_mutex_counter = 0;
    const int increments_per_task = 5;
    const int num_tasks = 3;

    for (int i = 0; i < num_tasks; i++) {
        task_control_block* t = create_priv_kernel_task(mutex_increment_task, (void*)&increments_per_task);
        ASSERT_TRUE(t != nullptr, "Should create mutex increment task");
        sched::scheduler::get().add_task(t);
    }

    // Run yields to let tasks finish
    // Each does increments_per_task increments, total is num_tasks * increments_per_task
    for (int i = 0; i < num_tasks * increments_per_task * 2; i++) {
        yield();
    }
    
    // Make sure all the tasks on all cpus fully finish within a 1 second interval
    sleep(1);

    ASSERT_EQ(global_mutex_counter, num_tasks * increments_per_task, "Mutex-protected increments should match the total expected");
    return UNIT_TEST_SUCCESS;
}

// Test that calling yield with no tasks doesn't crash and returns to same place
DECLARE_UNIT_TEST("multithread yield no tasks", test_yield_no_tasks) {
    // Assume no tasks are scheduled right now
    // Just call yield a few times
    yield();
    yield();
    // If we get here without crashing, test passes
    return UNIT_TEST_SUCCESS;
}

// Test that exiting a thread actually removes it from scheduling
DECLARE_UNIT_TEST("multithread exit_thread removal", test_exit_thread_removal) {
    global_counter = 0;

    // This task increments once and then exits
    auto single_increment_and_exit = [](void*) {
        g_multithreading_test_counter_lock.lock();
        global_counter++;
        g_multithreading_test_counter_lock.unlock();

        exit_thread();
    };

    task_control_block* t = create_priv_kernel_task(single_increment_and_exit, nullptr);
    sched::scheduler::get().add_task(t);

    // Yield a couple times to let the task run and exit
    yield();
    yield();

    // Make sure all the tasks on all cpus fully finish within a 1 second interval
    sleep(1);

    // The task should have incremented once and exited
    ASSERT_EQ(global_counter, 1, "Task should have incremented once before exiting");
    // Yield again - task should not run again
    yield();
    ASSERT_EQ(global_counter, 1, "No additional increments should occur after exit");

    return UNIT_TEST_SUCCESS;
}
