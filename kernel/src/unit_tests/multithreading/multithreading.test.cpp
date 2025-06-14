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

    exit_process();
}

// A task that immediately exits
void exit_immediately_task(void* data) {
    __unused data;
    exit_process();
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
    exit_process();
}

// Function that increments once and exits
void single_increment_and_exit(void* data) {
    __unused data;
    g_multithreading_test_counter_lock.lock();
    global_counter++;
    g_multithreading_test_counter_lock.unlock();
    exit_process();
}

// Test creating a single task and letting it run and exit
DECLARE_UNIT_TEST("multithread single task run and exit", test_single_task_run) {
    global_counter = 0;

    int increments = 10;
    process* proc = new process();
    ASSERT_TRUE(proc != nullptr, "process allocation failed");
    
    // Initialize as a privileged kernel process
    const auto proc_flags =
        process_creation_flags::IS_KERNEL |
        process_creation_flags::PRIV_KERN_THREAD |
        process_creation_flags::SCHEDULE_NOW; // This will place the task onto a random CPU

    bool ret = proc->init_with_entry("", increment_task, &increments, proc_flags);

    ASSERT_TRUE(ret, "process initialization failed");

    // Run the scheduler by calling yield until the task is done
    // Assuming that after 20 yields, the task will finish increments
    for (int i = 0; i < 20; i++) {
        yield();
    }

    // Make sure all the tasks on all cpus fully finish within a 1 second interval
    sleep(1);

    // After the task finishes (calls exit_process), it should not run again
    // Check the counter
    ASSERT_EQ(global_counter, increments, "The global counter didn't match increments count");

    return UNIT_TEST_SUCCESS;
}

// Test creating multiple tasks and letting them run and exit
DECLARE_UNIT_TEST("multithread multiple tasks run and exit", test_multiple_tasks) {
    global_counter = 0;
    int increments_per_task = 5;
    const int num_tasks = 4;

    const auto proc_flags =
        process_creation_flags::IS_KERNEL |
        process_creation_flags::PRIV_KERN_THREAD |
        process_creation_flags::SCHEDULE_NOW; // This will place the task onto a random CPU

    process* procs[num_tasks];
    for (int i = 0; i < num_tasks; i++) {
        procs[i] = new process();
        ASSERT_TRUE(procs[i] != nullptr, "process allocation failed");

        bool ret = procs[i]->init_with_entry("", increment_task, &increments_per_task, proc_flags);
        
        // Initialize as a privileged kernel process
        ASSERT_TRUE(ret,  "process initialization failed");
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

    const auto proc_flags =
        process_creation_flags::IS_KERNEL |
        process_creation_flags::PRIV_KERN_THREAD;

    for (int cpu_id = 0; cpu_id < num_tasks; cpu_id++) {
        int* data = (int*)zmalloc(sizeof(int));
        *data = increments_per_task;
        process* proc = new process();
        ASSERT_TRUE(proc != nullptr, "process allocation failed");

        // Initialize as a privileged kernel process
        bool ret = proc->init_with_entry("", increment_task, data, proc_flags);

        ASSERT_TRUE(ret, "process initialization failed");

        // Schedule the process on the specified CPU
        sched::scheduler::get().add_process(proc, cpu_id);
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
    process* proc = new process();
    ASSERT_TRUE(proc != nullptr, "process allocation failed");

    const auto proc_flags =
        process_creation_flags::IS_KERNEL |
        process_creation_flags::PRIV_KERN_THREAD;

    bool ret = proc->init_with_entry("", exit_immediately_task, nullptr, proc_flags);
    
    // Initialize as a privileged kernel process
    ASSERT_TRUE(ret, "process initialization failed");

    // Schedule the process on CPU 0
    sched::scheduler::get().add_process(proc, 0);

    // Just yield a few times, the task should exit immediately
    yield();
    yield();

    // No global state to check here, just ensure no crash
    return UNIT_TEST_SUCCESS;
}

// Test destroying a task that was never run (just to confirm resource cleanup)
DECLARE_UNIT_TEST("multithread destroy/cleanup task before scheduling", test_destroy_before_run) {
    process* proc = new process();
    ASSERT_TRUE(proc != nullptr, "process allocation failed");

    const auto proc_flags =
        process_creation_flags::IS_KERNEL |
        process_creation_flags::PRIV_KERN_THREAD;

    bool ret = proc->init_with_entry("", exit_immediately_task, nullptr, proc_flags);
    
    // Initialize as a privileged kernel process
    ASSERT_TRUE(ret, "process initialization failed");

    // Destroy the process without scheduling it
    proc->cleanup();
    delete proc;

    // No crash, no double free expected
    return UNIT_TEST_SUCCESS;
}

// Test that mutexes work correctly with multiple tasks
DECLARE_UNIT_TEST("multithread mutex usage", test_mutex_usage) {
    global_mutex_counter = 0;
    int num_tasks = 4;
    int increments_per_task = 10;

    const auto proc_flags =
        process_creation_flags::IS_KERNEL |
        process_creation_flags::PRIV_KERN_THREAD |
        process_creation_flags::SCHEDULE_NOW; // This will place the task onto a random CPU

    process* procs[num_tasks];
    for (int i = 0; i < num_tasks; i++) {
        procs[i] = new process();
        ASSERT_TRUE(procs[i] != nullptr, "process allocation failed");

        // Initialize as a privileged kernel process
        bool ret = procs[i]->init_with_entry("", mutex_increment_task, &increments_per_task, proc_flags);
        
        ASSERT_TRUE(ret, "process initialization failed");
    }

    // Run yields to let tasks finish
    for (int i = 0; i < num_tasks * increments_per_task * 2; i++) {
        yield();
    }
    
    // Make sure all the tasks on all cpus fully finish within a 1 second interval
    sleep(1);

    ASSERT_EQ(global_mutex_counter, num_tasks * increments_per_task, "All tasks should have incremented the counter with mutex protection");

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

// Test that exiting a process actually removes it from scheduling
DECLARE_UNIT_TEST("multithread exit process removal", test_exit_process_removal) {
    global_counter = 0;

    process* proc = new process();
    ASSERT_TRUE(proc != nullptr, "process allocation failed");

    const auto proc_flags =
        process_creation_flags::IS_KERNEL |
        process_creation_flags::PRIV_KERN_THREAD |
        process_creation_flags::SCHEDULE_NOW; // This will place the task onto a random CPU
    
    // Initialize as a privileged kernel process
    bool ret = proc->init_with_entry("", single_increment_and_exit, nullptr, proc_flags);

    ASSERT_TRUE(ret, "process initialization failed");

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
