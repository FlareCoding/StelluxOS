#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <stlxstd/thread.h>
#include <stlxstd/mutex.h>
#include <stlxstd/condition_variable.h>
#include <stlxstd/barrier.h>

static int g_passed = 0;
static int g_failed = 0;

static void check(const char* name, bool ok) {
    printf("  %s: %s\n", ok ? "PASS" : "FAIL", name);
    if (ok) g_passed++;
    else g_failed++;
}

// --- Test 1: Mutex stress ---

static constexpr int MUTEX_THREADS = 8;
static constexpr int MUTEX_ITERS = 10000;

static void test_mutex_stress() {
    stlxstd::mutex mtx;
    int counter = 0;

    stlxstd::thread threads[MUTEX_THREADS];
    for (int i = 0; i < MUTEX_THREADS; i++) {
        threads[i] = stlxstd::thread([&] {
            for (int j = 0; j < MUTEX_ITERS; j++) {
                stlxstd::lock_guard<stlxstd::mutex> guard(mtx);
                counter++;
            }
        });
    }
    for (int i = 0; i < MUTEX_THREADS; i++) {
        threads[i].join();
    }

    check("mutex stress (8 threads x 10000)", counter == MUTEX_THREADS * MUTEX_ITERS);
}

// --- Test 2: Condition variable producer/consumer ---

static constexpr int ITEMS = 100;

static void test_condvar_producer_consumer() {
    stlxstd::mutex mtx;
    stlxstd::condition_variable cv;
    int produced = 0;
    int consumed = 0;
    bool done = false;

    stlxstd::thread consumer([&] {
        stlxstd::unique_lock<stlxstd::mutex> lock(mtx);
        while (!done || produced > consumed) {
            cv.wait(lock, [&] { return produced > consumed || done; });
            while (produced > consumed) {
                consumed++;
            }
        }
    });

    stlxstd::thread producer([&] {
        for (int i = 0; i < ITEMS; i++) {
            {
                stlxstd::lock_guard<stlxstd::mutex> guard(mtx);
                produced++;
            }
            cv.notify_one();
        }
        {
            stlxstd::lock_guard<stlxstd::mutex> guard(mtx);
            done = true;
        }
        cv.notify_one();
    });

    producer.join();
    consumer.join();

    check("condvar producer/consumer (100 items)", consumed == ITEMS);
}

// --- Test 3: Barrier synchronization ---

static constexpr int BARRIER_THREADS = 4;

static void test_barrier() {
    stlxstd::barrier bar(BARRIER_THREADS);
    volatile int flags[BARRIER_THREADS] = {};
    volatile int verified[BARRIER_THREADS] = {};

    stlxstd::thread threads[BARRIER_THREADS];
    for (int i = 0; i < BARRIER_THREADS; i++) {
        threads[i] = stlxstd::thread([&, i] {
            __atomic_store_n(&flags[i], 1, __ATOMIC_RELEASE);
            bar.arrive_and_wait();
            // After barrier: all flags must be set
            bool all_set = true;
            for (int j = 0; j < BARRIER_THREADS; j++) {
                if (!__atomic_load_n(&flags[j], __ATOMIC_ACQUIRE)) {
                    all_set = false;
                }
            }
            __atomic_store_n(&verified[i], all_set ? 1 : 0, __ATOMIC_RELEASE);
        });
    }
    for (int i = 0; i < BARRIER_THREADS; i++) {
        threads[i].join();
    }

    bool ok = true;
    for (int i = 0; i < BARRIER_THREADS; i++) {
        if (!verified[i]) ok = false;
    }
    check("barrier (4 threads)", ok);
}

// --- Test 4: Thread join ---

static void test_thread_join() {
    volatile int value = 0;

    stlxstd::thread t([&] {
        __atomic_store_n(&value, 42, __ATOMIC_RELEASE);
    });
    t.join();

    check("thread join", __atomic_load_n(&value, __ATOMIC_ACQUIRE) == 42);
}

// --- Test 5: Thread detach ---

static void test_thread_detach() {
    volatile int flag = 0;

    {
        stlxstd::thread t([&] {
            __atomic_store_n(&flag, 1, __ATOMIC_RELEASE);
        });
        t.detach();
    }

    // Brief busy-wait for the detached thread to run
    for (int i = 0; i < 10000000; i++) {
        if (__atomic_load_n(&flag, __ATOMIC_ACQUIRE)) break;
        asm volatile("" ::: "memory");
    }

    check("thread detach", __atomic_load_n(&flag, __ATOMIC_ACQUIRE) == 1);
}

// --- Test 6: Multiple independent mutexes ---

static constexpr int MULTI_THREADS = 4;
static constexpr int MULTI_ITERS = 5000;

static void test_multi_mutex() {
    stlxstd::mutex mtx_a, mtx_b;
    int counter_a = 0;
    int counter_b = 0;

    stlxstd::thread threads[MULTI_THREADS];
    for (int i = 0; i < MULTI_THREADS; i++) {
        threads[i] = stlxstd::thread([&] {
            for (int j = 0; j < MULTI_ITERS; j++) {
                {
                    stlxstd::lock_guard<stlxstd::mutex> guard(mtx_a);
                    counter_a++;
                }
                {
                    stlxstd::lock_guard<stlxstd::mutex> guard(mtx_b);
                    counter_b++;
                }
            }
        });
    }
    for (int i = 0; i < MULTI_THREADS; i++) {
        threads[i].join();
    }

    bool ok = (counter_a == MULTI_THREADS * MULTI_ITERS) &&
              (counter_b == MULTI_THREADS * MULTI_ITERS);
    check("multi-mutex (2 locks, 4 threads x 5000)", ok);
}

int main() {
    printf("\nsynctest: Stellux synchronization test suite\n\n");

    printf("[mutex]\n");
    test_mutex_stress();

    printf("\n[condition variable]\n");
    test_condvar_producer_consumer();

    printf("\n[barrier]\n");
    test_barrier();

    printf("\n[thread lifecycle]\n");
    test_thread_join();
    test_thread_detach();

    printf("\n[multi-mutex]\n");
    test_multi_mutex();

    printf("\n--- Results: %d passed, %d failed ---\n\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
