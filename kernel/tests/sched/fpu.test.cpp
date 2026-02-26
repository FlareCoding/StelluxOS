#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "smp/smp.h"
#include "dynpriv/dynpriv.h"

using test_helpers::spin_wait;
using test_helpers::spin_wait_ge;

TEST_SUITE(fpu);

extern "C" {
#ifdef __x86_64__
void fpu_test_write_xmm0(uint64_t value);
uint64_t fpu_test_read_xmm0();
#endif
#ifdef __aarch64__
void fpu_test_write_v0(uint64_t value);
uint64_t fpu_test_read_v0();
#endif
}

static void write_fpu_reg(uint64_t value) {
#ifdef __x86_64__
    fpu_test_write_xmm0(value);
#endif
#ifdef __aarch64__
    fpu_test_write_v0(value);
#endif
}

static uint64_t read_fpu_reg() {
#ifdef __x86_64__
    return fpu_test_read_xmm0();
#endif
#ifdef __aarch64__
    return fpu_test_read_v0();
#endif
}

// --- fpu_basic_float ---
// Task writes a known value to an FPU register and reads it back.

static volatile uint64_t g_basic_result = 0;
static volatile uint32_t g_basic_done = 0;

static void basic_float_fn(void*) {
    constexpr uint64_t pattern = 0xDEADBEEFCAFEBABEULL;
    RUN_ELEVATED({
        write_fpu_reg(pattern);
    });
    uint64_t readback = 0;
    RUN_ELEVATED({
        readback = read_fpu_reg();
    });
    __atomic_store_n(&g_basic_result, readback, __ATOMIC_RELEASE);
    __atomic_store_n(&g_basic_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(fpu, fpu_basic_float) {
    g_basic_result = 0;
    g_basic_done = 0;

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(
            basic_float_fn, nullptr, "fpu_basic");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(&g_basic_done));
    EXPECT_EQ(__atomic_load_n(&g_basic_result, __ATOMIC_ACQUIRE),
              0xDEADBEEFCAFEBABEULL);
}

// --- fpu_context_isolation ---
// Two tasks on the same CPU write different fingerprints to the same
// FPU register, yield repeatedly, and verify their value survives.

constexpr uint32_t ISOLATION_ITERS = 50;

static volatile uint32_t g_iso_fail_a = 0;
static volatile uint32_t g_iso_fail_b = 0;
static volatile uint32_t g_iso_done_count = 0;

static void isolation_task_fn(void* arg) {
    uint64_t fingerprint = reinterpret_cast<uint64_t>(arg);
    volatile uint32_t* fail_flag = (fingerprint == 0xAAAAAAAAAAAAAAAAULL)
        ? &g_iso_fail_a : &g_iso_fail_b;

    for (uint32_t i = 0; i < ISOLATION_ITERS; i++) {
        RUN_ELEVATED({
            write_fpu_reg(fingerprint);
        });
        sched::yield();
        uint64_t readback = 0;
        RUN_ELEVATED({
            readback = read_fpu_reg();
        });
        if (readback != fingerprint) {
            __atomic_store_n(fail_flag, 1, __ATOMIC_RELEASE);
            break;
        }
    }
    __atomic_fetch_add(&g_iso_done_count, 1, __ATOMIC_ACQ_REL);
    sched::exit(0);
}

TEST(fpu, fpu_context_isolation) {
    g_iso_fail_a = 0;
    g_iso_fail_b = 0;
    g_iso_done_count = 0;

    RUN_ELEVATED({
        sched::task* ta = sched::create_kernel_task(
            isolation_task_fn,
            reinterpret_cast<void*>(0xAAAAAAAAAAAAAAAAULL),
            "fpu_iso_a");
        sched::task* tb = sched::create_kernel_task(
            isolation_task_fn,
            reinterpret_cast<void*>(0xBBBBBBBBBBBBBBBBULL),
            "fpu_iso_b");
        ASSERT_NOT_NULL(ta);
        ASSERT_NOT_NULL(tb);
        sched::enqueue_on(ta, 0);
        sched::enqueue_on(tb, 0);
    });

    ASSERT_TRUE(spin_wait_ge(&g_iso_done_count, 2));
    EXPECT_EQ(__atomic_load_n(&g_iso_fail_a, __ATOMIC_ACQUIRE), 0u);
    EXPECT_EQ(__atomic_load_n(&g_iso_fail_b, __ATOMIC_ACQUIRE), 0u);
}

// --- fpu_cross_cpu ---
// Tasks on different CPUs each write a fingerprint and verify it.

static volatile uint32_t g_cross_fail = 0;
static volatile uint32_t g_cross_done_count = 0;

static void cross_cpu_fpu_fn(void* arg) {
    uint64_t fingerprint = reinterpret_cast<uint64_t>(arg);

    for (uint32_t i = 0; i < ISOLATION_ITERS; i++) {
        RUN_ELEVATED({
            write_fpu_reg(fingerprint);
        });
        sched::yield();
        uint64_t readback = 0;
        RUN_ELEVATED({
            readback = read_fpu_reg();
        });
        if (readback != fingerprint) {
            __atomic_store_n(&g_cross_fail, 1, __ATOMIC_RELEASE);
            break;
        }
    }
    __atomic_fetch_add(&g_cross_done_count, 1, __ATOMIC_ACQ_REL);
    sched::exit(0);
}

TEST(fpu, fpu_cross_cpu) {
    if (smp::cpu_count() < 2) return;

    g_cross_fail = 0;
    g_cross_done_count = 0;

    RUN_ELEVATED({
        sched::task* t0 = sched::create_kernel_task(
            cross_cpu_fpu_fn,
            reinterpret_cast<void*>(0x1111111111111111ULL),
            "fpu_xcpu0");
        sched::task* t1 = sched::create_kernel_task(
            cross_cpu_fpu_fn,
            reinterpret_cast<void*>(0x2222222222222222ULL),
            "fpu_xcpu1");
        ASSERT_NOT_NULL(t0);
        ASSERT_NOT_NULL(t1);
        sched::enqueue_on(t0, 0);
        sched::enqueue_on(t1, 1);
    });

    ASSERT_TRUE(spin_wait_ge(&g_cross_done_count, 2));
    EXPECT_EQ(__atomic_load_n(&g_cross_fail, __ATOMIC_ACQUIRE), 0u);
}
