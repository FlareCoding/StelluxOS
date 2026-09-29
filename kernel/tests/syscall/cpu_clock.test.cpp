#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_clock.h"
#include "clock/clock.h"
#include "exec/elf.h"
#include "resource/providers/proc_provider.h"
#include "sched/sched.h"
#include "sched/task.h"

using test_helpers::user_page;
using test_helpers::user_space_scope;

TEST_SUITE(cpu_clock_syscall);

struct kernel_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

static constexpr const char* PROGRAM_PATH = "/bin/hello";
static constexpr uint64_t CLOCK_PROCESS_CPUTIME_ID = 2;
static constexpr uint64_t CLOCK_THREAD_CPUTIME_ID = 3;
static constexpr uint64_t NS_PER_SEC = 1000000000ULL;
static constexpr uint64_t SPIN_NS = 20ULL * 1000 * 1000;
static constexpr uint32_t MISSING_ID = 0x0FFFFFF0;

// The low bits of an encoded CPU clock id, as pthread_getcpuclockid and
// clock_getcpuclockid build them
static constexpr int32_t THREAD_CLOCK_BITS = 6;
static constexpr int32_t PROCESS_CLOCK_BITS = 2;
static constexpr int32_t INVALID_KIND_BITS = 3;

static uint64_t cpu_clock_id(uint32_t id, int32_t low_bits) {
    int32_t encoded = static_cast<int32_t>(~id << 3) | low_bits;
    return static_cast<uint64_t>(static_cast<int64_t>(encoded));
}

static int64_t read_clock(user_page& page, uint64_t clock_id, uint64_t* out_ns) {
    int64_t rc = 0;
    {
        user_space_scope scope(page.ctx);
        rc = sys_clock_gettime(clock_id, page.addr, 0, 0, 0, 0);
    }

    auto* ts = page.at<kernel_timespec>(0);
    *out_ns = static_cast<uint64_t>(ts->tv_sec) * NS_PER_SEC + static_cast<uint64_t>(ts->tv_nsec);
    return rc;
}

static int64_t read_resolution(user_page& page, uint64_t clock_id, int64_t* out_ns) {
    int64_t rc = 0;
    {
        user_space_scope scope(page.ctx);
        rc = sys_clock_getres(clock_id, page.addr, 0, 0, 0, 0);
    }

    *out_ns = page.at<kernel_timespec>(0)->tv_nsec;
    return rc;
}

static void spin_for_ns(uint64_t duration_ns) {
    uint64_t deadline = clock::now_ns() + duration_ns;
    while (clock::now_ns() < deadline) {
    }
}

TEST(cpu_clock_syscall, the_thread_clock_advances_while_the_thread_runs) {
    user_page page;
    ASSERT_TRUE(page.ready());

    uint64_t wall_start = clock::now_ns();
    uint64_t before = 0;
    ASSERT_EQ(read_clock(page, CLOCK_THREAD_CPUTIME_ID, &before), 0);

    spin_for_ns(SPIN_NS);

    uint64_t after = 0;
    ASSERT_EQ(read_clock(page, CLOCK_THREAD_CPUTIME_ID, &after), 0);
    uint64_t wall_ns = clock::now_ns() - wall_start;

    EXPECT_GE(after - before, SPIN_NS / 2);
    EXPECT_LE(after - before, wall_ns);
}

TEST(cpu_clock_syscall, the_process_clock_covers_the_calling_thread) {
    user_page page;
    ASSERT_TRUE(page.ready());

    uint64_t thread_ns = 0;
    uint64_t process_ns = 0;
    ASSERT_EQ(read_clock(page, CLOCK_THREAD_CPUTIME_ID, &thread_ns), 0);
    ASSERT_EQ(read_clock(page, CLOCK_PROCESS_CPUTIME_ID, &process_ns), 0);

    EXPECT_GE(process_ns, thread_ns);
}

TEST(cpu_clock_syscall, a_zero_id_names_the_caller) {
    user_page page;
    ASSERT_TRUE(page.ready());

    uint64_t via_zero = 0;
    uint64_t direct = 0;
    ASSERT_EQ(read_clock(page, cpu_clock_id(0, THREAD_CLOCK_BITS), &via_zero), 0);
    ASSERT_EQ(read_clock(page, CLOCK_THREAD_CPUTIME_ID, &direct), 0);

    EXPECT_GT(via_zero, static_cast<uint64_t>(0));
    EXPECT_GE(direct, via_zero);
}

TEST(cpu_clock_syscall, another_process_can_be_read_but_not_its_threads) {
    exec::loaded_image loaded;
    ASSERT_EQ(exec::load_elf(PROGRAM_PATH, &loaded), exec::OK);
    sched::task* child = sched::create_user_task(&loaded, PROGRAM_PATH);
    ASSERT_NOT_NULL(child);

    user_page page;
    ASSERT_TRUE(page.ready());

    uint64_t process_ns = 1;
    uint64_t thread_ns = 0;
    EXPECT_EQ(read_clock(page, cpu_clock_id(child->group->pid, PROCESS_CLOCK_BITS), &process_ns), 0);
    EXPECT_EQ(read_clock(page, cpu_clock_id(child->tid, THREAD_CLOCK_BITS), &thread_ns), syscall::EINVAL);
    EXPECT_EQ(process_ns, static_cast<uint64_t>(0));

    resource::proc_provider::destroy_unstarted_task(child);
}

static void exit_at_once(void*) {
    sched::exit(0);
}

TEST(cpu_clock_syscall, a_kernel_task_is_not_a_process) {
    sched::task* worker = sched::create_kernel_task(exit_at_once, nullptr, "test_clock_kernel");
    ASSERT_NOT_NULL(worker);

    user_page page;
    ASSERT_TRUE(page.ready());

    uint64_t unused = 0;
    EXPECT_EQ(read_clock(page, cpu_clock_id(worker->tid, PROCESS_CLOCK_BITS), &unused), syscall::EINVAL);
    EXPECT_EQ(read_clock(page, cpu_clock_id(worker->tid, THREAD_CLOCK_BITS), &unused), syscall::EINVAL);

    sched::enqueue(worker);
}

TEST(cpu_clock_syscall, ids_naming_no_task_or_no_clock_are_refused) {
    user_page page;
    ASSERT_TRUE(page.ready());

    uint64_t unused = 0;
    EXPECT_EQ(read_clock(page, cpu_clock_id(MISSING_ID, THREAD_CLOCK_BITS), &unused), syscall::EINVAL);
    EXPECT_EQ(read_clock(page, cpu_clock_id(MISSING_ID, PROCESS_CLOCK_BITS), &unused), syscall::EINVAL);
    EXPECT_EQ(read_clock(page, cpu_clock_id(0, INVALID_KIND_BITS), &unused), syscall::EINVAL);
}

TEST(cpu_clock_syscall, cpu_clocks_report_nanosecond_resolution) {
    user_page page;
    ASSERT_TRUE(page.ready());

    int64_t thread_res = 0;
    int64_t process_res = 0;
    int64_t missing_res = 0;
    EXPECT_EQ(read_resolution(page, CLOCK_THREAD_CPUTIME_ID, &thread_res), 0);
    EXPECT_EQ(read_resolution(page, CLOCK_PROCESS_CPUTIME_ID, &process_res), 0);
    EXPECT_EQ(read_resolution(page, cpu_clock_id(MISSING_ID, PROCESS_CLOCK_BITS), &missing_res), syscall::EINVAL);

    EXPECT_EQ(thread_res, 1);
    EXPECT_EQ(process_res, 1);
}
