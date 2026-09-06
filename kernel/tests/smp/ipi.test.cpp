#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "smp/ipi.h"
#include "smp/smp.h"
#include "percpu/percpu.h"
#include "dynpriv/dynpriv.h"

using test_helpers::spin_wait_ge;
using test_helpers::brief_delay;

TEST_SUITE(ipi);

// One probe message for the whole suite, registered once because slots are
// never returned. The handler records where it ran and how often.
static smp::ipi::message g_probe = {0};
static sync::atomic<uint32_t> g_probe_count;
static sync::atomic<uint32_t> g_probe_cpu{0xFFFFFFFF};

static void probe_handler() {
    g_probe_cpu.store_release(percpu::current_cpu_id());
    g_probe_count.fetch_add_release(1);
}

static int32_t register_probe() {
    int32_t rc = smp::ipi::OK;
    RUN_ELEVATED(rc = smp::ipi::register_message(probe_handler, &g_probe));
    return rc;
}

BEFORE_ALL(ipi, register_probe);

static void reset_probe() {
    g_probe_count.store_relaxed(0);
    g_probe_cpu.store_relaxed(0xFFFFFFFF);
}

// First online CPU other than the caller, or the caller's own id if none
static uint32_t pick_other_online_cpu() {
    uint32_t self = percpu::current_cpu_id();
    for (uint32_t cpu = 0; cpu < smp::cpu_count(); cpu++) {
        smp::cpu_info* info = smp::get_cpu_info(cpu);
        if (cpu != self && info->state.load_acquire() == smp::CPU_ONLINE) {
            return cpu;
        }
    }

    return self;
}

TEST(ipi, rejects_bad_arguments) {
    int32_t rc = smp::ipi::OK;
    smp::ipi::message unused = {0};

    RUN_ELEVATED(rc = smp::ipi::register_message(nullptr, &unused));
    EXPECT_EQ(rc, smp::ipi::ERR_INVALID);

    RUN_ELEVATED(rc = smp::ipi::send(0, smp::ipi::message{0}));
    EXPECT_EQ(rc, smp::ipi::ERR_INVALID);

    RUN_ELEVATED(rc = smp::ipi::send(smp::cpu_count(), g_probe));
    EXPECT_EQ(rc, smp::ipi::ERR_OFFLINE);
}

TEST(ipi, delivers_to_self) {
    uint32_t self = percpu::current_cpu_id();
    reset_probe();

    int32_t rc = smp::ipi::OK;
    RUN_ELEVATED(rc = smp::ipi::send(self, g_probe));
    ASSERT_EQ(rc, smp::ipi::OK);

    ASSERT_TRUE(spin_wait_ge(g_probe_count, 1));
    EXPECT_EQ(g_probe_cpu.load_acquire(), self);
}

TEST(ipi, delivers_to_another_cpu) {
    uint32_t target = pick_other_online_cpu();
    if (target == percpu::current_cpu_id()) return;

    reset_probe();

    int32_t rc = smp::ipi::OK;
    RUN_ELEVATED(rc = smp::ipi::send(target, g_probe));
    ASSERT_EQ(rc, smp::ipi::OK);

    ASSERT_TRUE(spin_wait_ge(g_probe_count, 1));
    EXPECT_EQ(g_probe_cpu.load_acquire(), target);
}

TEST(ipi, reaches_every_other_cpu_exactly_once) {
    reset_probe();

    uint32_t sent = 0;
    RUN_ELEVATED(sent = smp::ipi::send_all_but_self(g_probe));
    EXPECT_EQ(sent, smp::online_count() - 1);
    if (sent == 0) return;

    ASSERT_TRUE(spin_wait_ge(g_probe_count, sent));
    brief_delay();
    EXPECT_EQ(g_probe_count.load_acquire(), sent);
}
