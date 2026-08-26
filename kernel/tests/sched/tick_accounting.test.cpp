#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "smp/smp.h"
#include "clock/clock.h"
#include "percpu/percpu.h"
#include "dynpriv/dynpriv.h"

using test_helpers::spin_wait;

TEST_SUITE(tick_accounting);

static constexpr uint64_t SPIN_NS = 150ULL * 1000 * 1000;

static uint64_t total_ticks_all_cpus() {
    uint64_t total = 0;
    for (uint32_t cpu = 0; cpu < smp::cpu_count(); cpu++) {
        sched::cpu_accounting_stats stats =
            sched::read_cpu_accounting_stats(cpu);
        total += stats.busy_ticks + stats.idle_ticks;
    }
    return total;
}

static uint64_t busy_ticks_all_cpus() {
    uint64_t total = 0;
    for (uint32_t cpu = 0; cpu < smp::cpu_count(); cpu++) {
        total += sched::read_cpu_accounting_stats(cpu).busy_ticks;
    }
    return total;
}

static void spin_for_ns(uint64_t duration_ns) {
    uint64_t deadline = clock::now_ns() + duration_ns;
    while (clock::now_ns() < deadline) {
    }
}

// The spinner runs for a fixed duration and reports its own run_ticks
// before exiting, because its task struct is reaped after exit
static volatile uint32_t g_spinner_done = 0;
static volatile uint32_t g_spinner_ticks = 0;

static void spinner_task_fn(void*) {
    spin_for_ns(SPIN_NS);
    RUN_ELEVATED({
        sched::task* self = sched::current();
        uint64_t ticks = self->run_ticks.load_relaxed();
        __atomic_store_n(&g_spinner_ticks, static_cast<uint32_t>(ticks),
                         __ATOMIC_RELEASE);
    });
    __atomic_store_n(&g_spinner_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

// --- ticks_advance ---
// Proves: timer ticks keep landing in the per-CPU busy or idle
// counters while wall clock time passes.

TEST(tick_accounting, ticks_advance) {
    uint64_t before = 0;
    uint64_t after = 0;

    RUN_ELEVATED({
        before = total_ticks_all_cpus();
    });

    // Spin well past several 100 Hz timer periods
    spin_for_ns(SPIN_NS);

    RUN_ELEVATED({
        after = total_ticks_all_cpus();
    });

    EXPECT_LT(before, after);
}

// --- busy_task_charged ---
// Proves: a compute-bound task accumulates run_ticks while current
// and its CPU charges that time as busy.

TEST(tick_accounting, busy_task_charged) {
    g_spinner_done = 0;
    g_spinner_ticks = 0;

    uint64_t busy_before = 0;
    RUN_ELEVATED({
        busy_before = busy_ticks_all_cpus();

        sched::task* spinner = sched::create_kernel_task(
            spinner_task_fn, nullptr, "test_tick_spin");
        ASSERT_NOT_NULL(spinner);

        // Place the spinner away from this CPU so it runs even while
        // this task keeps spinning in spin_wait below
        uint32_t cpu_count = smp::cpu_count();
        if (cpu_count > 1) {
            uint32_t target = (percpu::current_cpu_id() + 1) % cpu_count;
            sched::enqueue_on(spinner, target);
        } else {
            sched::enqueue(spinner);
        }
    });

    ASSERT_TRUE(spin_wait(&g_spinner_done));

    uint64_t busy_after = 0;
    RUN_ELEVATED({
        busy_after = busy_ticks_all_cpus();
    });

    EXPECT_LT(busy_before, busy_after);
    EXPECT_TRUE(g_spinner_ticks > 0);
}
