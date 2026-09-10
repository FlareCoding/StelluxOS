#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "smp/smp.h"
#include "percpu/percpu.h"
#include "dynpriv/dynpriv.h"
#include "sync/seqlock.h"

using test_helpers::spin_wait;
using test_helpers::spin_wait_ge;

TEST_SUITE(seqlock);

constexpr uint32_t MAX_TEST_CPUS = 16;
constexpr uint64_t WRITE_ROUNDS = 20000;

// A pair is consistent only when both halves come from the same write
struct mirrored_pair {
    uint64_t value;
    uint64_t mirror;
};

static bool consistent(const mirrored_pair& p) {
    return p.mirror == ~p.value;
}

static sync::seqlocked<mirrored_pair> g_pair({0, ~0ULL});
static sync::atomic<uint32_t> g_readers_started;
static sync::atomic<uint32_t> g_stop;
static sync::atomic<uint32_t> g_torn_reads;
static sync::atomic<uint64_t> g_reads;
static sync::atomic<uint32_t> g_reader_done[MAX_TEST_CPUS] = {};

static void reader_fn(void* arg) {
    uint32_t idx = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    g_readers_started.fetch_add_release(1);

    while (!g_stop.load_acquire()) {
        mirrored_pair p = g_pair.read();
        if (!consistent(p)) {
            g_torn_reads.fetch_add_relaxed(1);
        }
        g_reads.fetch_add_relaxed(1);
    }

    g_reader_done[idx].store_release(1);
    sched::exit(0);
}

// --- write_then_read_returns_the_value ---
// Proves: without contention a read returns exactly what was last written.

TEST(seqlock, write_then_read_returns_the_value) {
    sync::seqlocked<mirrored_pair> pair({1, ~1ULL});
    mirrored_pair p = pair.read();
    EXPECT_EQ(p.value, 1ULL);
    EXPECT_TRUE(consistent(p));

    pair.write({42, ~42ULL});
    p = pair.read();
    EXPECT_EQ(p.value, 42ULL);
    EXPECT_TRUE(consistent(p));
}

// --- readers_never_see_a_torn_write ---
// Proves: readers on every other CPU only ever observe whole writes while
// this CPU rewrites both halves thousands of times.

TEST(seqlock, readers_never_see_a_torn_write) {
    uint32_t cpus = smp::cpu_count();
    if (cpus < 2 || cpus > MAX_TEST_CPUS) {
        return;
    }

    g_pair.write({0, ~0ULL});
    g_readers_started.store_relaxed(0);
    g_stop.store_relaxed(0);
    g_torn_reads.store_relaxed(0);
    g_reads.store_relaxed(0);
    for (uint32_t cpu = 0; cpu < cpus; cpu++) {
        g_reader_done[cpu].store_relaxed(0);
    }

    uint32_t self = percpu::current_cpu_id();
    RUN_ELEVATED({
        for (uint32_t cpu = 0; cpu < cpus; cpu++) {
            if (cpu == self) {
                continue;
            }

            sched::task* t = sched::create_kernel_task(
                reader_fn, reinterpret_cast<void*>(static_cast<uintptr_t>(cpu)), "seq_reader");
            ASSERT_NOT_NULL(t);
            sched::enqueue_on(t, cpu);
        }
    });

    ASSERT_TRUE(spin_wait_ge(g_readers_started, cpus - 1));

    for (uint64_t round = 1; round <= WRITE_ROUNDS; round++) {
        g_pair.write({round, ~round});
    }

    g_stop.store_release(1);
    for (uint32_t cpu = 0; cpu < cpus; cpu++) {
        if (cpu != self) {
            ASSERT_TRUE(spin_wait(g_reader_done[cpu]));
        }
    }

    EXPECT_EQ(g_torn_reads.load_acquire(), 0u);
    EXPECT_NE(g_reads.load_acquire(), 0ULL);
    EXPECT_EQ(g_pair.read().value, WRITE_ROUNDS);
}
