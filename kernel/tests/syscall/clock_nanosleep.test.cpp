#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_clock.h"
#include "syscall/syscall_table.h"
#include "clock/clock.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "dynpriv/dynpriv.h"

using test_helpers::user_page;
using test_helpers::user_space_scope;

TEST_SUITE(clock_nanosleep_syscall);

struct sleep_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

// A request on a clock, for tests that walk several
struct clock_request {
    uint64_t       clock_id;
    sleep_timespec request;
};

// One clock_nanosleep or nanosleep call, made from a task of its own since the test body must never block
struct sleep_call {
    user_page*             page;
    bool                   is_nanosleep;
    uint64_t               clock_id;
    uint64_t               flags;
    uint64_t               remainder_at;
    int64_t                result;
    uint64_t               started_ns;
    uint64_t               returned_ns;
    sync::atomic<uint32_t> done;
};

constexpr uint64_t CLOCK_REALTIME           = 0;
constexpr uint64_t CLOCK_MONOTONIC          = 1;
constexpr uint64_t CLOCK_PROCESS_CPUTIME_ID = 2;
constexpr uint64_t CLOCK_THREAD_CPUTIME_ID  = 3;
constexpr uint64_t CLOCK_MONOTONIC_RAW      = 4;
constexpr uint64_t CLOCK_BOOTTIME           = 7;
constexpr uint64_t TIMER_ABSTIME            = 1;
constexpr uint64_t UNKNOWN_FLAG             = 2;
constexpr uint64_t HIGHEST_UNKNOWN_FLAG     = 1ULL << 63;

// A negative id names a task's CPU clock, this one the calling thread's
constexpr uint64_t CALLER_THREAD_CPU_CLOCK = static_cast<uint64_t>(-2);

constexpr int64_t  NS_PER_SEC = 1000000000;
constexpr size_t   REQUEST_AT = 0;
constexpr size_t   REMAINDER_AT = 64;

// A remainder offset for which the call passes no remainder address
constexpr uint64_t NO_REMAINDER = ~0ULL;

// Long enough to tell a sleep from an immediate return, and the most any such sleep may overrun its request
constexpr uint64_t SHORT_SLEEP_NS = 50000000;
constexpr uint64_t MAX_SLEEP_OVERRUN_NS = 2000000000;

// A sleep that the test interrupts long before it ends, and how long an interrupted sleeper sleeps first
constexpr int64_t  LONG_SLEEP_SEC = 10;
constexpr uint64_t INTERRUPT_AFTER_NS = 200000000;

// 2^64 nanoseconds, one past the longest time 64 bits of nanoseconds can count
constexpr sleep_timespec PAST_THE_CLOCK_RANGE = {18446744073, 709551616};

// The most seconds a timespec holds, the usual request for a sleep with no end
constexpr int64_t LONGEST_SECONDS = 0x7FFFFFFFFFFFFFFF;

static sleep_call g_call;

static sleep_timespec to_timespec(uint64_t ns) {
    return {static_cast<int64_t>(ns / NS_PER_SEC), static_cast<int64_t>(ns % NS_PER_SEC)};
}

static uint64_t to_ns(const sleep_timespec& ts) {
    return static_cast<uint64_t>(ts.tv_sec) * NS_PER_SEC + static_cast<uint64_t>(ts.tv_nsec);
}

static void run_sleep_call(void* arg) {
    auto& call = *static_cast<sleep_call*>(arg);
    uint64_t u_req = call.page->addr + REQUEST_AT;
    uint64_t u_rem = call.remainder_at == NO_REMAINDER ? 0 : call.page->addr + call.remainder_at;
    {
        user_space_scope scope(call.page->ctx);
        call.started_ns = clock::now_ns();
        call.result = call.is_nanosleep ? sys_nanosleep(u_req, u_rem, 0, 0, 0, 0)
                                        : sys_clock_nanosleep(call.clock_id, call.flags, u_req, u_rem, 0, 0);
        call.returned_ns = clock::now_ns();
    }

    call.done.store_release(1);
    sched::exit(0);
}

// Starts a task that calls clock_nanosleep with the request in `page`, writing any remainder at `remainder_at`,
// pinned until `finish_sleep` or `interrupt_sleep` unpins it
static sched::task* start_sleep(user_page& page, uint64_t clock_id, uint64_t flags,
                                uint64_t remainder_at = REMAINDER_AT) {
    *page.at<sleep_timespec>(REMAINDER_AT) = {-1, -1};

    g_call.page = &page;
    g_call.is_nanosleep = false;
    g_call.clock_id = clock_id;
    g_call.flags = flags;
    g_call.remainder_at = remainder_at;
    g_call.done.store_relaxed(0);

    return test_helpers::start_pinned_task(run_sleep_call, &g_call, "clock_nanosleep");
}

// Starts a task that calls nanosleep with the request in `page`, pinned as `start_sleep`'s is
static sched::task* start_nanosleep(user_page& page, uint64_t remainder_at = REMAINDER_AT) {
    *page.at<sleep_timespec>(REMAINDER_AT) = {-1, -1};

    g_call.page = &page;
    g_call.is_nanosleep = true;
    g_call.remainder_at = remainder_at;
    g_call.done.store_relaxed(0);

    return test_helpers::start_pinned_task(run_sleep_call, &g_call, "nanosleep");
}

// Waits for the sleep to end on its own, false when a kill had to end it
static bool finish_sleep(sched::task* sleeper) {
    bool finished = test_helpers::finish_or_kill(sleeper, g_call.done);
    test_helpers::unpin(sleeper);

    return finished;
}

// Interrupts a sleep once its task has slept `asleep_ns`, false when the task was not seen blocked or did not return
static bool interrupt_sleep(sched::task* sleeper, uint64_t asleep_ns) {
    bool blocked = test_helpers::blocks_before_deadline(sleeper);
    uint64_t blocked_at = clock::now_ns();
    while (clock::now_ns() - blocked_at < asleep_ns) {
        test_helpers::brief_delay();
    }

    RUN_ELEVATED(sched::force_wake_for_kill(sleeper));
    bool returned = test_helpers::spin_wait(g_call.done);
    test_helpers::unpin(sleeper);

    return blocked && returned;
}

// A call made from the test body, for requests refused before any sleep
static int64_t call_from_test_body(user_page& page, uint64_t clock_id, uint64_t flags, uint64_t u_req) {
    user_space_scope scope(page.ctx);
    return sys_clock_nanosleep(clock_id, flags, u_req, page.addr + REMAINDER_AT, 0, 0);
}

TEST(clock_nanosleep_syscall, a_relative_sleep_lasts_at_least_its_request) {
    user_page page;
    ASSERT_TRUE(page.ready());

    const uint64_t clocks[] = {CLOCK_MONOTONIC, CLOCK_REALTIME, CLOCK_BOOTTIME};
    for (uint64_t clock_id : clocks) {
        *page.at<sleep_timespec>(REQUEST_AT) = to_timespec(SHORT_SLEEP_NS);
        sched::task* sleeper = start_sleep(page, clock_id, 0);
        ASSERT_NOT_NULL(sleeper);
        ASSERT_TRUE(finish_sleep(sleeper));

        EXPECT_EQ(g_call.result, static_cast<int64_t>(0));
        EXPECT_GE(g_call.returned_ns - g_call.started_ns, SHORT_SLEEP_NS);
        EXPECT_LE(g_call.returned_ns - g_call.started_ns, SHORT_SLEEP_NS + MAX_SLEEP_OVERRUN_NS);
    }
}

TEST(clock_nanosleep_syscall, an_absolute_sleep_wakes_once_its_clock_reaches_the_request) {
    user_page page;
    ASSERT_TRUE(page.ready());

    const uint64_t monotonic_clocks[] = {CLOCK_MONOTONIC, CLOCK_BOOTTIME};
    for (uint64_t clock_id : monotonic_clocks) {
        uint64_t monotonic_request = clock::now_ns() + SHORT_SLEEP_NS;
        *page.at<sleep_timespec>(REQUEST_AT) = to_timespec(monotonic_request);
        sched::task* sleeper = start_sleep(page, clock_id, TIMER_ABSTIME);
        ASSERT_NOT_NULL(sleeper);
        ASSERT_TRUE(finish_sleep(sleeper));

        EXPECT_EQ(g_call.result, static_cast<int64_t>(0));
        EXPECT_GE(g_call.returned_ns, monotonic_request);
        EXPECT_LE(g_call.returned_ns, monotonic_request + MAX_SLEEP_OVERRUN_NS);
    }

    uint64_t realtime_request = clock::realtime_ns() + SHORT_SLEEP_NS;
    *page.at<sleep_timespec>(REQUEST_AT) = to_timespec(realtime_request);
    sched::task* sleeper = start_sleep(page, CLOCK_REALTIME, TIMER_ABSTIME);
    ASSERT_NOT_NULL(sleeper);
    ASSERT_TRUE(finish_sleep(sleeper));

    uint64_t realtime_returned = clock::boot_realtime_ns() + g_call.returned_ns;
    EXPECT_EQ(g_call.result, static_cast<int64_t>(0));
    EXPECT_GE(realtime_returned, realtime_request);
    EXPECT_LE(realtime_returned, realtime_request + MAX_SLEEP_OVERRUN_NS);
}

TEST(clock_nanosleep_syscall, an_absolute_time_already_passed_returns_at_once) {
    user_page page;
    ASSERT_TRUE(page.ready());

    // The last nanosecond of the first second since boot, and a realtime second long before boot
    const clock_request passed_requests[] = {
        {CLOCK_MONOTONIC, to_timespec(clock::now_ns() - 1)},
        {CLOCK_BOOTTIME, to_timespec(clock::now_ns() - 1)},
        {CLOCK_REALTIME, to_timespec(clock::realtime_ns() - 1)},
        {CLOCK_MONOTONIC, {0, NS_PER_SEC - 1}},
        {CLOCK_REALTIME, {1, 0}},
    };

    for (const clock_request& passed : passed_requests) {
        *page.at<sleep_timespec>(REQUEST_AT) = passed.request;
        sched::task* sleeper = start_sleep(page, passed.clock_id, TIMER_ABSTIME);
        ASSERT_NOT_NULL(sleeper);
        ASSERT_TRUE(finish_sleep(sleeper));

        EXPECT_EQ(g_call.result, static_cast<int64_t>(0));
        EXPECT_LE(g_call.returned_ns - g_call.started_ns, MAX_SLEEP_OVERRUN_NS);
    }
}

TEST(clock_nanosleep_syscall, an_interrupted_relative_sleep_reports_the_time_left) {
    user_page page;
    ASSERT_TRUE(page.ready());

    const bool through_nanosleep[] = {false, true};
    for (bool is_nanosleep : through_nanosleep) {
        *page.at<sleep_timespec>(REQUEST_AT) = {LONG_SLEEP_SEC, 0};
        sched::task* sleeper = is_nanosleep ? start_nanosleep(page) : start_sleep(page, CLOCK_MONOTONIC, 0);
        ASSERT_NOT_NULL(sleeper);
        ASSERT_TRUE(interrupt_sleep(sleeper, INTERRUPT_AFTER_NS));

        const sleep_timespec left = *page.at<sleep_timespec>(REMAINDER_AT);
        uint64_t requested_ns = LONG_SLEEP_SEC * NS_PER_SEC;
        EXPECT_EQ(g_call.result, syscall::EINTR);
        EXPECT_TRUE(left.tv_nsec >= 0 && left.tv_nsec < NS_PER_SEC);
        EXPECT_LE(to_ns(left), requested_ns - INTERRUPT_AFTER_NS);
        EXPECT_GE(to_ns(left), requested_ns - (g_call.returned_ns - g_call.started_ns));
    }
}

TEST(clock_nanosleep_syscall, an_interrupted_sleep_without_a_remainder_address_fails_with_eintr) {
    user_page page;
    ASSERT_TRUE(page.ready());

    const bool through_nanosleep[] = {false, true};
    for (bool is_nanosleep : through_nanosleep) {
        *page.at<sleep_timespec>(REQUEST_AT) = {LONG_SLEEP_SEC, 0};
        sched::task* sleeper =
            is_nanosleep ? start_nanosleep(page, NO_REMAINDER) : start_sleep(page, CLOCK_MONOTONIC, 0, NO_REMAINDER);
        ASSERT_NOT_NULL(sleeper);
        ASSERT_TRUE(interrupt_sleep(sleeper, 0));

        EXPECT_EQ(g_call.result, syscall::EINTR);
    }
}

TEST(clock_nanosleep_syscall, an_interrupted_absolute_sleep_writes_no_remainder) {
    user_page page;
    ASSERT_TRUE(page.ready());

    *page.at<sleep_timespec>(REQUEST_AT) = to_timespec(clock::now_ns() + LONG_SLEEP_SEC * NS_PER_SEC);
    sched::task* sleeper = start_sleep(page, CLOCK_MONOTONIC, TIMER_ABSTIME);
    ASSERT_NOT_NULL(sleeper);
    ASSERT_TRUE(interrupt_sleep(sleeper, 0));

    EXPECT_EQ(g_call.result, syscall::EINTR);
    EXPECT_EQ(page.at<sleep_timespec>(REMAINDER_AT)->tv_sec, static_cast<int64_t>(-1));
    EXPECT_EQ(page.at<sleep_timespec>(REMAINDER_AT)->tv_nsec, static_cast<int64_t>(-1));
}

TEST(clock_nanosleep_syscall, an_interrupted_sleep_with_an_unwritable_remainder_fails_with_efault) {
    user_page page;
    ASSERT_TRUE(page.ready());

    // The page is the address space's only mapping, so the address past it is unmapped
    *page.at<sleep_timespec>(REQUEST_AT) = {LONG_SLEEP_SEC, 0};
    sched::task* sleeper = start_sleep(page, CLOCK_MONOTONIC, 0, pmm::PAGE_SIZE);
    ASSERT_NOT_NULL(sleeper);
    ASSERT_TRUE(interrupt_sleep(sleeper, 0));

    EXPECT_EQ(g_call.result, syscall::EFAULT);
}

TEST(clock_nanosleep_syscall, a_request_past_the_clock_range_sleeps_until_interrupted) {
    user_page page;
    ASSERT_TRUE(page.ready());

    const uint64_t flags[] = {0, TIMER_ABSTIME};
    for (uint64_t flag : flags) {
        *page.at<sleep_timespec>(REQUEST_AT) = PAST_THE_CLOCK_RANGE;
        sched::task* sleeper = start_sleep(page, CLOCK_MONOTONIC, flag);
        ASSERT_NOT_NULL(sleeper);
        bool slept = test_helpers::stays_asleep(sleeper);
        ASSERT_TRUE(interrupt_sleep(sleeper, 0));

        EXPECT_TRUE(slept);
        EXPECT_EQ(g_call.result, syscall::EINTR);
    }
}

TEST(clock_nanosleep_syscall, nanosleep_for_the_most_seconds_sleeps_until_interrupted) {
    user_page page;
    ASSERT_TRUE(page.ready());

    *page.at<sleep_timespec>(REQUEST_AT) = {LONGEST_SECONDS, 0};
    sched::task* sleeper = start_nanosleep(page);
    ASSERT_NOT_NULL(sleeper);
    bool slept = test_helpers::stays_asleep(sleeper);
    ASSERT_TRUE(interrupt_sleep(sleeper, 0));

    EXPECT_TRUE(slept);
    EXPECT_EQ(g_call.result, syscall::EINTR);
}

TEST(clock_nanosleep_syscall, nanosleep_is_a_relative_sleep) {
    user_page page;
    ASSERT_TRUE(page.ready());

    *page.at<sleep_timespec>(REQUEST_AT) = to_timespec(SHORT_SLEEP_NS);
    sched::task* sleeper = start_nanosleep(page);
    ASSERT_NOT_NULL(sleeper);
    ASSERT_TRUE(finish_sleep(sleeper));

    EXPECT_EQ(g_call.result, static_cast<int64_t>(0));
    EXPECT_GE(g_call.returned_ns - g_call.started_ns, SHORT_SLEEP_NS);
    EXPECT_LE(g_call.returned_ns - g_call.started_ns, SHORT_SLEEP_NS + MAX_SLEEP_OVERRUN_NS);
}

TEST(clock_nanosleep_syscall, unsupported_clocks_and_flags_are_refused) {
    user_page page;
    ASSERT_TRUE(page.ready());

    *page.at<sleep_timespec>(REQUEST_AT) = {0, 1};

    const uint64_t clocks[] = {
        CLOCK_PROCESS_CPUTIME_ID, CLOCK_THREAD_CPUTIME_ID, CLOCK_MONOTONIC_RAW, CALLER_THREAD_CPU_CLOCK,
    };

    for (uint64_t clock_id : clocks) {
        EXPECT_EQ(call_from_test_body(page, clock_id, 0, page.addr + REQUEST_AT), syscall::EINVAL);
    }

    EXPECT_EQ(call_from_test_body(page, CLOCK_MONOTONIC, UNKNOWN_FLAG, page.addr + REQUEST_AT), syscall::EINVAL);
    EXPECT_EQ(call_from_test_body(page, CLOCK_MONOTONIC, HIGHEST_UNKNOWN_FLAG, page.addr + REQUEST_AT),
              syscall::EINVAL);
    EXPECT_EQ(call_from_test_body(page, CLOCK_REALTIME, TIMER_ABSTIME | UNKNOWN_FLAG, page.addr + REQUEST_AT),
              syscall::EINVAL);
}

TEST(clock_nanosleep_syscall, out_of_range_and_unreadable_requests_are_refused) {
    user_page page;
    ASSERT_TRUE(page.ready());

    const sleep_timespec out_of_range[] = {{0, -1}, {0, NS_PER_SEC}, {-1, 0}};
    for (const sleep_timespec& request : out_of_range) {
        *page.at<sleep_timespec>(REQUEST_AT) = request;
        EXPECT_EQ(call_from_test_body(page, CLOCK_MONOTONIC, 0, page.addr + REQUEST_AT), syscall::EINVAL);
        EXPECT_EQ(call_from_test_body(page, CLOCK_REALTIME, TIMER_ABSTIME, page.addr + REQUEST_AT), syscall::EINVAL);
    }

    EXPECT_EQ(call_from_test_body(page, CLOCK_MONOTONIC, 0, 0), syscall::EFAULT);
    EXPECT_EQ(call_from_test_body(page, CLOCK_MONOTONIC, 0, page.addr + pmm::PAGE_SIZE), syscall::EFAULT);
}
