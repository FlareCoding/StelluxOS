#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "smp/smp.h"
#include "clock/clock.h"
#include "percpu/percpu.h"
#include "mm/heap.h"
#include "dynpriv/dynpriv.h"

using test_helpers::spin_wait;

TEST_SUITE(cpu_time);

static constexpr uint64_t NS_PER_MS = 1000ULL * 1000;
static constexpr uint64_t LONG_SPIN_NS = 150 * NS_PER_MS;
static constexpr uint64_t SHORT_SPIN_NS = 3 * NS_PER_MS; // shorter than one timer tick
static constexpr uint64_t SLEEP_MS = 50;
static constexpr uint64_t SLEEP_CHARGE_LIMIT_NS = 5 * NS_PER_MS;

static void spin_for_ns(uint64_t duration_ns) {
    uint64_t deadline = clock::now_ns() + duration_ns;
    while (clock::now_ns() < deadline) {
    }
}

// Placing a worker on another CPU lets it run while this task spins in spin_wait
static void enqueue_away_from_this_cpu(sched::task* t) {
    uint32_t cpu_count = smp::cpu_count();
    if (cpu_count > 1) {
        sched::enqueue_on(t, (percpu::current_cpu_id() + 1) % cpu_count);
    } else {
        sched::enqueue(t);
    }
}

static uint64_t busy_ns_all_cpus() {
    uint64_t total = 0;
    for (uint32_t cpu = 0; cpu < smp::cpu_count(); cpu++) {
        total += sched::read_cpu_accounting_stats(cpu).busy_ns;
    }

    return total;
}

static uint64_t accounted_ns_all_cpus() {
    uint64_t total = 0;
    for (uint32_t cpu = 0; cpu < smp::cpu_count(); cpu++) {
        sched::cpu_accounting_stats stats = sched::read_cpu_accounting_stats(cpu);
        total += stats.busy_ns + stats.idle_ns;
    }

    return total;
}

// A worker measures its own CPU and wall time and reports them before
// exiting, because its task is reaped after exit
static void (*g_worker_work)(uint64_t amount);
static uint64_t g_worker_amount;
static uint64_t g_worker_cpu_ns;
static uint64_t g_worker_wall_ns;
static sync::atomic<uint32_t> g_worker_done;

static void measured_worker(void*) {
    uint64_t wall_start = clock::now_ns();
    uint64_t cpu_start = 0;
    RUN_ELEVATED({
        cpu_start = sched::read_task_cpu_time_ns(sched::current());
    });

    g_worker_work(g_worker_amount);

    uint64_t cpu_end = 0;
    RUN_ELEVATED({
        cpu_end = sched::read_task_cpu_time_ns(sched::current());
    });

    g_worker_wall_ns = clock::now_ns() - wall_start;
    g_worker_cpu_ns = cpu_end - cpu_start;
    g_worker_done.store_release(1);
    sched::exit(0);
}

static bool run_measured_worker(void (*work)(uint64_t amount), uint64_t amount) {
    g_worker_work = work;
    g_worker_amount = amount;
    g_worker_done.store_relaxed(0);

    bool started = false;
    RUN_ELEVATED({
        sched::task* worker = sched::create_kernel_task(measured_worker, nullptr, "test_cpu_time");
        if (worker) {
            enqueue_away_from_this_cpu(worker);
            started = true;
        }
    });

    return started && spin_wait(g_worker_done);
}

static void spin_work(uint64_t duration_ns) {
    spin_for_ns(duration_ns);
}

static void sleep_work(uint64_t duration_ms) {
    RUN_ELEVATED({
        sched::sleep_ms(duration_ms);
    });
}

TEST(cpu_time, a_spinning_task_is_charged_the_time_it_spins) {
    ASSERT_TRUE(run_measured_worker(spin_work, LONG_SPIN_NS));

    EXPECT_LE(g_worker_cpu_ns, g_worker_wall_ns);
    EXPECT_GE(g_worker_cpu_ns, g_worker_wall_ns / 2);
}

TEST(cpu_time, a_run_shorter_than_a_tick_is_charged_its_real_time) {
    ASSERT_TRUE(run_measured_worker(spin_work, SHORT_SPIN_NS));

    EXPECT_LE(g_worker_cpu_ns, g_worker_wall_ns);
    EXPECT_GE(g_worker_cpu_ns, g_worker_wall_ns / 2);
}

TEST(cpu_time, a_sleeping_task_is_not_charged) {
    ASSERT_TRUE(run_measured_worker(sleep_work, SLEEP_MS));

    EXPECT_GE(g_worker_wall_ns, SLEEP_MS * NS_PER_MS);
    EXPECT_LT(g_worker_cpu_ns, SLEEP_CHARGE_LIMIT_NS);
}

TEST(cpu_time, cpu_accounted_time_advances_with_the_clock) {
    uint64_t before = 0;
    RUN_ELEVATED({
        before = accounted_ns_all_cpus();
    });

    spin_for_ns(LONG_SPIN_NS);

    uint64_t after = 0;
    RUN_ELEVATED({
        after = accounted_ns_all_cpus();
    });

    EXPECT_GE(after - before, LONG_SPIN_NS / 2);
}

TEST(cpu_time, a_spinning_task_is_charged_as_busy_time) {
    uint64_t busy_before = 0;
    RUN_ELEVATED({
        busy_before = busy_ns_all_cpus();
    });

    ASSERT_TRUE(run_measured_worker(spin_work, LONG_SPIN_NS));

    uint64_t busy_after = 0;
    RUN_ELEVATED({
        busy_after = busy_ns_all_cpus();
    });

    EXPECT_GE(busy_after - busy_before, g_worker_cpu_ns / 2);
}

static sched::thread_group* g_fold_group;

static void group_spin_worker(void*) {
    spin_for_ns(LONG_SPIN_NS);
    sched::exit(0);
}

// A task that is dead and off its CPU has used all the CPU time it ever will
static bool wait_until_finished(sched::task* t) {
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    while (true) {
        bool finished = false;
        RUN_ELEVATED({
            finished = t->state.load_acquire() == sched::TASK_STATE_DEAD &&
                       sync::atomic_ref<uint32_t>{t->exec.on_cpu}.load_acquire() == 0;
        });

        if (finished) {
            return true;
        }

        if (clock::now_ns() > deadline) {
            return false;
        }
    }
}

TEST(cpu_time, a_thread_leaving_its_group_hands_it_all_of_its_cpu_time) {
    rc::strong_ref<sched::task> worker;
    RUN_ELEVATED({
        g_fold_group = heap::kalloc_new<sched::thread_group>();
        sched::task* created = g_fold_group
            ? sched::create_kernel_task(group_spin_worker, nullptr, "test_group_spin")
            : nullptr;

        if (created) {
            g_fold_group->lock = sync::SPINLOCK_INIT;
            g_fold_group->threads.init();

            // The worker's reference, which the reaper releases
            g_fold_group->add_ref();
            created->group = g_fold_group;
            g_fold_group->threads.push_back(created);
            g_fold_group->thread_count = 1;

            // Holding the task keeps its final CPU time readable after it exits
            worker = sched::task_ref(created);
            enqueue_away_from_this_cpu(created);
        }
    });

    ASSERT_TRUE(static_cast<bool>(worker));
    ASSERT_TRUE(wait_until_finished(worker.ptr()));

    uint64_t group_cpu_ns = 0;
    uint64_t worker_cpu_ns = 0;
    RUN_ELEVATED({
        group_cpu_ns = sched::read_group_cpu_time_ns(g_fold_group);
        worker_cpu_ns = worker->cpu_time_ns.load_relaxed();
        worker.reset();
        if (g_fold_group->release()) {
            sched::thread_group::ref_destroy(g_fold_group);
        }
    });

    EXPECT_GE(group_cpu_ns, LONG_SPIN_NS / 2);
    EXPECT_EQ(group_cpu_ns, worker_cpu_ns);
}
