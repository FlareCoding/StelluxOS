#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_task.h"
#include "smp/smp.h"
#include "percpu/percpu.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "dynpriv/dynpriv.h"

using test_helpers::spin_wait;
using test_helpers::user_page;
using test_helpers::user_space_scope;

TEST_SUITE(affinity_syscall);

// The C library passes a cpu_set_t, room for 1024 CPUs
constexpr uint64_t LIBC_CPU_SET_BYTES = 128;
constexpr uint32_t BITS_PER_BYTE      = 8;
constexpr uint64_t UNUSED_TID         = 0x7FFFFFFF; // Far beyond any id a test run hands out
constexpr uint64_t TARGET_POLL_NS     = 1000000;

static int64_t getaffinity_into(user_page& page, uint64_t pid, uint64_t size) {
    user_space_scope scope(page.ctx);
    return sys_sched_getaffinity(pid, size, page.addr, 0, 0, 0);
}

static uint32_t bits_set(const uint8_t* bytes, int64_t count) {
    uint32_t bits = 0;
    for (int64_t i = 0; i < count; i++) {
        bits += static_cast<uint32_t>(__builtin_popcount(bytes[i]));
    }
    return bits;
}

TEST(affinity_syscall, reports_every_online_cpu) {
    user_page page;
    ASSERT_TRUE(page.ready());

    int64_t rc = getaffinity_into(page, 0, LIBC_CPU_SET_BYTES);
    ASSERT_TRUE(rc > 0);
    EXPECT_EQ(rc % static_cast<int64_t>(sizeof(uint64_t)), 0);

    uint32_t online = smp::online_count();
    EXPECT_EQ(bits_set(page.at<uint8_t>(0), rc), online ? online : 1u);

    uint32_t self = percpu::current_cpu_id();
    EXPECT_TRUE((page.at<uint8_t>(0)[self / BITS_PER_BYTE] >> (self % BITS_PER_BYTE)) & 1u);
}

static sync::atomic<uint32_t> g_target_parked;
static sync::atomic<uint32_t> g_target_release;
static sync::atomic<uint32_t> g_target_done;

// Stays alive until released, so its thread id resolves while the test asks about it
static void parked_target_fn(void*) {
    g_target_parked.store_release(1);
    while (!g_target_release.load_acquire()) {
        RUN_ELEVATED({
            sched::sleep_ns(TARGET_POLL_NS);
        });
    }

    g_target_done.store_release(1);
    sched::exit(0);
}

TEST(affinity_syscall, a_live_thread_id_names_that_thread) {
    g_target_parked.store_relaxed(0);
    g_target_release.store_relaxed(0);
    g_target_done.store_relaxed(0);

    sched::task* t = nullptr;
    rc::strong_ref<sched::task> pin;
    RUN_ELEVATED({
        t = sched::create_kernel_task(parked_target_fn, nullptr, "affinity_target");
        if (t) {
            pin = sched::task_ref(t);
            sched::enqueue(t);
        }
    });
    ASSERT_NOT_NULL(t);
    ASSERT_TRUE(spin_wait(g_target_parked));

    user_page page;
    int64_t rc = page.ready() ? getaffinity_into(page, t->tid, LIBC_CPU_SET_BYTES) : 0;

    g_target_release.store_release(1);
    ASSERT_TRUE(spin_wait(g_target_done));
    EXPECT_TRUE(page.ready());
    EXPECT_TRUE(rc > 0);
}

TEST(affinity_syscall, unknown_thread_ids_are_refused) {
    user_page page;
    ASSERT_TRUE(page.ready());

    EXPECT_EQ(getaffinity_into(page, UNUSED_TID, LIBC_CPU_SET_BYTES), syscall::ESRCH);
    EXPECT_EQ(getaffinity_into(page, static_cast<uint64_t>(-1), LIBC_CPU_SET_BYTES), syscall::ESRCH);
}

TEST(affinity_syscall, sizes_that_cannot_hold_the_mask_are_refused) {
    user_page page;
    ASSERT_TRUE(page.ready());

    EXPECT_EQ(getaffinity_into(page, 0, 4), syscall::EINVAL);
    EXPECT_EQ(getaffinity_into(page, 0, sizeof(uint64_t) + 4), syscall::EINVAL);
}

TEST(affinity_syscall, an_unmapped_buffer_faults) {
    user_page page;
    ASSERT_TRUE(page.ready());

    int64_t rc = 0;
    {
        user_space_scope scope(page.ctx);
        rc = sys_sched_getaffinity(0, LIBC_CPU_SET_BYTES, 0, 0, 0, 0);
    }
    EXPECT_EQ(rc, syscall::EFAULT);
}
