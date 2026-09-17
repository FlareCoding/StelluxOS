#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "timer/timer.h"
#include "clock/clock.h"
#include "percpu/percpu.h"
#include "sched/sched.h"
#include "smp/smp.h"
#include "sync/atomic.h"
#include "dynpriv/dynpriv.h"
#include "hw/cpu.h"

TEST_SUITE(deadline_timer);

using test_helpers::spin_wait;

constexpr uint64_t MS = 1000000ULL;
constexpr size_t   MAX_PROBES = 8;

// A timer under test, the deadline_timer first so the callback recovers it by cast
struct probe {
    timer::deadline_timer timer;
    uint32_t              id;
    uint32_t              runs;
    uint32_t              ran_on_cpu;
    uint64_t              ran_at_ns;
    bool                  cancel_self_result;
};

// Virtual deadlines sit an hour past the real clock, where only the test's own
// expiry calls can reach them and the interrupt never considers them due
static uint64_t g_base_ns;
static uint32_t g_order[MAX_PROBES];
static uint32_t g_order_count;

static probe* probe_of(timer::deadline_timer* self) {
    return reinterpret_cast<probe*>(self);
}

static void record_run(timer::deadline_timer* self) {
    probe* p = probe_of(self);
    p->runs++;
    p->ran_on_cpu = percpu::current_cpu_id();
    p->ran_at_ns = clock::now_ns();
    if (g_order_count < MAX_PROBES) {
        g_order[g_order_count++] = p->id;
    }
}

static void clear(probe& p, timer::deadline_fn fn, uint32_t id) {
    timer::init_deadline_timer(&p.timer, fn);
    p.id = id;
    p.runs = 0;
    p.ran_on_cpu = 0;
    p.ran_at_ns = 0;
    p.cancel_self_result = true;
}

static void reset(probe* probes, size_t count, timer::deadline_fn fn) {
    g_base_ns = clock::now_ns() + 3600ULL * 1000ULL * MS;
    g_order_count = 0;
    for (size_t i = 0; i < count; i++) {
        clear(probes[i], fn, static_cast<uint32_t>(i));
    }
}

static void schedule_at(probe& p, uint64_t offset_ns) {
    RUN_ELEVATED(timer::schedule(&p.timer, g_base_ns + offset_ns));
}

static void fire_at(uint64_t offset_ns) {
    RUN_ELEVATED(timer::__dbg_test_fire_expired(g_base_ns + offset_ns));
}

TEST(deadline_timer, runs_in_deadline_order) {
    probe probes[3];
    reset(probes, 3, record_run);

    schedule_at(probes[0], 30 * MS);
    schedule_at(probes[1], 10 * MS);
    schedule_at(probes[2], 20 * MS);

    fire_at(25 * MS);
    EXPECT_EQ(g_order_count, 2u);
    EXPECT_EQ(g_order[0], 1u);
    EXPECT_EQ(g_order[1], 2u);
    EXPECT_TRUE(timer::is_pending(&probes[0].timer));
    EXPECT_FALSE(timer::is_pending(&probes[1].timer));

    fire_at(40 * MS);
    EXPECT_EQ(g_order_count, 3u);
    EXPECT_EQ(g_order[2], 0u);
    EXPECT_FALSE(timer::is_pending(&probes[0].timer));
}

TEST(deadline_timer, equal_deadlines_run_in_schedule_order) {
    probe probes[3];
    reset(probes, 3, record_run);

    schedule_at(probes[2], 10 * MS);
    schedule_at(probes[0], 10 * MS);
    schedule_at(probes[1], 10 * MS);

    fire_at(10 * MS);
    EXPECT_EQ(g_order_count, 3u);
    EXPECT_EQ(g_order[0], 2u);
    EXPECT_EQ(g_order[1], 0u);
    EXPECT_EQ(g_order[2], 1u);
}

TEST(deadline_timer, schedule_moves_the_deadline) {
    probe probes[1];
    reset(probes, 1, record_run);

    schedule_at(probes[0], 10 * MS);
    schedule_at(probes[0], 50 * MS);

    fire_at(20 * MS);
    EXPECT_EQ(probes[0].runs, 0u);
    EXPECT_TRUE(timer::is_pending(&probes[0].timer));

    fire_at(60 * MS);
    EXPECT_EQ(probes[0].runs, 1u);
    EXPECT_FALSE(timer::is_pending(&probes[0].timer));
}

TEST(deadline_timer, cancel_prevents_the_callback) {
    probe probes[1];
    reset(probes, 1, record_run);

    bool cancelled = false;
    RUN_ELEVATED(cancelled = timer::cancel(&probes[0].timer));
    EXPECT_TRUE(cancelled);

    schedule_at(probes[0], 10 * MS);
    EXPECT_TRUE(timer::is_pending(&probes[0].timer));

    RUN_ELEVATED(cancelled = timer::cancel(&probes[0].timer));
    EXPECT_TRUE(cancelled);
    EXPECT_FALSE(timer::is_pending(&probes[0].timer));

    fire_at(20 * MS);
    EXPECT_EQ(probes[0].runs, 0u);
}

TEST(deadline_timer, cancel_after_the_deadline_passed_still_prevents_it) {
    probe probes[1];
    reset(probes, 1, record_run);

    schedule_at(probes[0], 10 * MS);

    // The deadline is behind us but the worker has not taken the timer yet
    bool cancelled = false;
    RUN_ELEVATED(cancelled = timer::cancel(&probes[0].timer));
    EXPECT_TRUE(cancelled);

    fire_at(30 * MS);
    EXPECT_EQ(probes[0].runs, 0u);
}

// --- a callback that schedules itself again ---

static void reschedule_self(timer::deadline_timer* self) {
    record_run(self);
    if (probe_of(self)->runs < 3) {
        RUN_ELEVATED(timer::schedule(self, self->deadline_ns + 10 * MS));
    }
}

TEST(deadline_timer, callback_scheduling_itself_waits_for_the_next_batch) {
    probe probes[1];
    reset(probes, 1, reschedule_self);

    schedule_at(probes[0], 10 * MS);

    // Every instance is already due, yet one batch runs exactly one of them
    fire_at(100 * MS);
    EXPECT_EQ(probes[0].runs, 1u);
    EXPECT_TRUE(timer::is_pending(&probes[0].timer));

    fire_at(100 * MS);
    EXPECT_EQ(probes[0].runs, 2u);

    fire_at(100 * MS);
    EXPECT_EQ(probes[0].runs, 3u);
    EXPECT_FALSE(timer::is_pending(&probes[0].timer));

    fire_at(100 * MS);
    EXPECT_EQ(probes[0].runs, 3u);
}

// --- a callback that cancels its neighbor, and one that cancels itself ---

static probe* g_neighbor;

static void cancel_neighbor(timer::deadline_timer* self) {
    record_run(self);
    RUN_ELEVATED((void)timer::cancel(&g_neighbor->timer));
}

TEST(deadline_timer, callback_can_cancel_a_timer_due_in_the_same_batch) {
    probe probes[2];
    reset(probes, 2, record_run);
    probes[0].timer.fn = cancel_neighbor;
    g_neighbor = &probes[1];

    schedule_at(probes[0], 10 * MS);
    schedule_at(probes[1], 10 * MS);

    fire_at(10 * MS);
    EXPECT_EQ(probes[0].runs, 1u);
    EXPECT_EQ(probes[1].runs, 0u);
    EXPECT_FALSE(timer::is_pending(&probes[1].timer));
}

static void cancel_self(timer::deadline_timer* self) {
    record_run(self);
    bool result = true;
    RUN_ELEVATED(result = timer::cancel(self));
    probe_of(self)->cancel_self_result = result;
}

TEST(deadline_timer, cancel_reports_false_while_the_callback_runs) {
    probe probes[1];
    reset(probes, 1, cancel_self);

    schedule_at(probes[0], 10 * MS);
    fire_at(10 * MS);

    EXPECT_EQ(probes[0].runs, 1u);
    EXPECT_FALSE(probes[0].cancel_self_result);
}

// --- real time, through the interrupt and the worker ---

static sync::atomic<uint32_t> g_real_done;
static probe g_real_probe;

static void real_run(timer::deadline_timer* self) {
    record_run(self);
    g_real_done.store_release(1);
}

TEST(deadline_timer, worker_runs_a_timer_at_its_deadline) {
    g_real_done.store_relaxed(0);
    clear(g_real_probe, real_run, 0);

    uint64_t start = clock::now_ns();
    RUN_ELEVATED(timer::schedule(&g_real_probe.timer, start + 20 * MS));

    ASSERT_TRUE(spin_wait(g_real_done));
    EXPECT_EQ(g_real_probe.runs, 1u);
    EXPECT_GE(g_real_probe.ran_at_ns, start + 20 * MS);
    EXPECT_LT(g_real_probe.ran_at_ns, start + 200 * MS);
    EXPECT_EQ(g_real_probe.ran_on_cpu, percpu::current_cpu_id());
}

// --- scheduling from another CPU ---

static sync::atomic<uint32_t> g_remote_scheduled;
static uint32_t g_remote_cpu;

static void remote_scheduler(void*) {
    g_remote_cpu = percpu::current_cpu_id();
    RUN_ELEVATED(timer::schedule(&g_real_probe.timer, clock::now_ns() + 20 * MS));
    g_remote_scheduled.store_release(1);
    sched::exit(0);
}

TEST(deadline_timer, timer_belongs_to_the_cpu_that_scheduled_it) {
    if (smp::cpu_count() < 2) return;

    g_real_done.store_relaxed(0);
    g_remote_scheduled.store_relaxed(0);
    clear(g_real_probe, real_run, 0);

    sched::task* remote = nullptr;
    RUN_ELEVATED({
        remote = sched::create_kernel_task(remote_scheduler, nullptr, "deadline_remote");
        ASSERT_NOT_NULL(remote);
        sched::enqueue_on(remote, 1);
    });

    ASSERT_TRUE(spin_wait(g_remote_scheduled));
    EXPECT_EQ(g_real_probe.timer.cpu.load_acquire(), g_remote_cpu);
    EXPECT_EQ(g_remote_cpu, 1u);

    ASSERT_TRUE(spin_wait(g_real_done));
    EXPECT_EQ(g_real_probe.ran_on_cpu, 1u);
}

TEST(deadline_timer, rescheduling_from_another_cpu_moves_the_timer) {
    if (smp::cpu_count() < 2) return;

    g_real_done.store_relaxed(0);
    g_remote_scheduled.store_relaxed(0);
    clear(g_real_probe, real_run, 0);

    sched::task* remote = nullptr;
    RUN_ELEVATED({
        remote = sched::create_kernel_task(remote_scheduler, nullptr, "deadline_remote");
        ASSERT_NOT_NULL(remote);
        sched::enqueue_on(remote, 1);
    });

    ASSERT_TRUE(spin_wait(g_remote_scheduled));
    EXPECT_EQ(g_real_probe.timer.cpu.load_acquire(), 1u);

    // Moving it here also pulls it forward, so it fires from this CPU's tree
    RUN_ELEVATED(timer::schedule(&g_real_probe.timer, clock::now_ns() + 5 * MS));
    EXPECT_EQ(g_real_probe.timer.cpu.load_acquire(), percpu::current_cpu_id());

    ASSERT_TRUE(spin_wait(g_real_done));
    EXPECT_EQ(g_real_probe.runs, 1u);
    EXPECT_EQ(g_real_probe.ran_on_cpu, percpu::current_cpu_id());
}

// --- two CPUs scheduling and cancelling the same timer at once ---

constexpr uint32_t RACE_ROUNDS = 2000;

static sync::atomic<uint32_t> g_race_done;
static sync::atomic<uint32_t> g_race_runs;

static void race_run(timer::deadline_timer*) {
    g_race_runs.fetch_add_relaxed(1);
}

static void race_remote(void*) {
    for (uint32_t i = 0; i < RACE_ROUNDS; i++) {
        RUN_ELEVATED(timer::schedule(&g_real_probe.timer, clock::now_ns() + MS));
    }

    g_race_done.store_release(1);
    sched::exit(0);
}

TEST(deadline_timer, schedule_and_cancel_race_across_cpus_leaves_a_consistent_timer) {
    if (smp::cpu_count() < 2) return;

    g_race_done.store_relaxed(0);
    g_race_runs.store_relaxed(0);
    clear(g_real_probe, race_run, 0);

    sched::task* remote = nullptr;
    RUN_ELEVATED({
        remote = sched::create_kernel_task(race_remote, nullptr, "deadline_race");
        ASSERT_NOT_NULL(remote);
        sched::enqueue_on(remote, 1);
    });

    for (uint32_t i = 0; i < RACE_ROUNDS; i++) {
        RUN_ELEVATED({
            (void)timer::cancel(&g_real_probe.timer);
            timer::schedule(&g_real_probe.timer, clock::now_ns() + 2 * MS);
        });
    }

    ASSERT_TRUE(spin_wait(g_race_done));

    // Whatever the interleaving, the timer ends in exactly one place or none.
    // A cancel that meets the callback mid-run is legitimate, so retry briefly.
    bool cancelled = false;
    uint64_t give_up = clock::now_ns() + 100 * MS;
    while (!cancelled && clock::now_ns() < give_up) {
        RUN_ELEVATED(cancelled = timer::cancel(&g_real_probe.timer));
    }

    EXPECT_TRUE(cancelled);
    EXPECT_FALSE(timer::is_pending(&g_real_probe.timer));

    uint64_t settle = clock::now_ns() + 20 * MS;
    while (clock::now_ns() < settle) {
        cpu::relax();
    }

    uint32_t runs_after_cancel = g_race_runs.load_acquire();
    settle = clock::now_ns() + 20 * MS;
    while (clock::now_ns() < settle) {
        cpu::relax();
    }
    EXPECT_EQ(g_race_runs.load_acquire(), runs_after_cancel);
}
