#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "epoll/epoll.h"
#include "resource/resource.h"
#include "sync/poll.h"
#include "sync/atomic.h"
#include "mm/heap.h"
#include "sched/sched.h"
#include "smp/smp.h"

using test_helpers::spin_wait;
using test_helpers::spin_wait_ge;

TEST_SUITE(epoll_interests);

constexpr resource::handle_t HANDLE         = 3;
constexpr resource::handle_t OTHER_HANDLE   = 4;
constexpr uint64_t           DATA           = 0xC0FFEE;
constexpr uint32_t           EXCLUSIVE      = 1u << 28;
constexpr uint32_t           WAKE_UP        = 1u << 29;
constexpr uint32_t           ONE_SHOT       = 1u << 30;
constexpr uint32_t           EDGE_TRIGGERED = 1u << 31;
constexpr uint32_t           RACE_ROUNDS    = 256;

// Off CPU 0, whose idle task runs the test body and would starve behind busy racers
constexpr uint32_t EPOLL_RACER_CPU  = 1;
constexpr uint32_t TARGET_RACER_CPU = 2;
constexpr uint32_t RACE_CPUS        = 3;

// Two tasks on their own CPUs destroy each round's epoll and target at the same moment
struct destroy_race {
    resource::resource_object* epolls[RACE_ROUNDS];
    resource::resource_object* targets[RACE_ROUNDS];
    sync::atomic<uint32_t>     arrivals;
    sync::atomic<uint32_t>     epolls_done;
    sync::atomic<uint32_t>     targets_done;
};

static uint32_t never_ready(resource::resource_object*, sync::poll_table*) {
    return 0;
}

// A pollable stand-in for any watched object
static const resource::resource_ops g_pollable_ops = {
    .poll = never_ready,
};

static const resource::resource_ops g_unpollable_ops = {};

static resource::resource_object* create_target(const resource::resource_ops* ops) {
    auto* obj = heap::kalloc_new<resource::resource_object>();
    if (obj) {
        obj->type = resource::resource_type::PIPE;
        obj->ops = ops;
    }

    return obj;
}

static resource::resource_object* create_epoll() {
    resource::resource_object* ep = nullptr;
    if (epoll::create(&ep) != epoll::OK) {
        return nullptr;
    }

    return ep;
}

TEST(epoll_interests, an_interest_is_added_modified_and_removed) {
    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_pollable_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);

    EXPECT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN, DATA), epoll::OK);
    EXPECT_EQ(epoll::interest_count(ep), 1u);

    EXPECT_EQ(epoll::modify_interest(ep, HANDLE, target, sync::POLL_OUT | sync::POLL_RDHUP, DATA + 1), epoll::OK);
    EXPECT_EQ(epoll::interest_count(ep), 1u);

    EXPECT_EQ(epoll::remove_interest(ep, HANDLE, target), epoll::OK);
    EXPECT_EQ(epoll::interest_count(ep), 0u);

    resource::resource_release(target);
    resource::resource_release(ep);
}

TEST(epoll_interests, an_interest_is_keyed_by_handle_and_object) {
    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_pollable_ops);
    resource::resource_object* other = create_target(&g_pollable_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);
    ASSERT_NOT_NULL(other);

    EXPECT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN, DATA), epoll::OK);
    EXPECT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN, DATA), epoll::ERR_EXIST);

    // The same object through another handle, and a reused handle number naming another object
    EXPECT_EQ(epoll::add_interest(ep, OTHER_HANDLE, target, sync::POLL_IN, DATA), epoll::OK);
    EXPECT_EQ(epoll::add_interest(ep, HANDLE, other, sync::POLL_IN, DATA), epoll::OK);
    EXPECT_EQ(epoll::interest_count(ep), 3u);

    EXPECT_EQ(epoll::remove_interest(ep, HANDLE, target), epoll::OK);
    EXPECT_EQ(epoll::remove_interest(ep, HANDLE, target), epoll::ERR_NOENT);
    EXPECT_EQ(epoll::modify_interest(ep, HANDLE, target, sync::POLL_OUT, DATA), epoll::ERR_NOENT);
    EXPECT_EQ(epoll::interest_count(ep), 2u);

    resource::resource_release(other);
    resource::resource_release(target);
    resource::resource_release(ep);
}

TEST(epoll_interests, unsupported_events_and_targets_are_refused) {
    resource::resource_object* ep = create_epoll();
    resource::resource_object* other_ep = create_epoll();
    resource::resource_object* target = create_target(&g_pollable_ops);
    resource::resource_object* unpollable = create_target(&g_unpollable_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(other_ep);
    ASSERT_NOT_NULL(target);
    ASSERT_NOT_NULL(unpollable);

    EXPECT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN | EDGE_TRIGGERED, DATA), epoll::ERR_INVAL);
    EXPECT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN | ONE_SHOT, DATA), epoll::ERR_INVAL);
    EXPECT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN | EXCLUSIVE, DATA), epoll::ERR_INVAL);
    EXPECT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN | WAKE_UP, DATA), epoll::ERR_INVAL);

    EXPECT_EQ(epoll::add_interest(ep, HANDLE, ep, sync::POLL_IN, DATA), epoll::ERR_INVAL);
    EXPECT_EQ(epoll::add_interest(ep, HANDLE, other_ep, sync::POLL_IN, DATA), epoll::ERR_INVAL);
    EXPECT_EQ(epoll::add_interest(target, HANDLE, unpollable, sync::POLL_IN, DATA), epoll::ERR_INVAL);
    EXPECT_EQ(epoll::add_interest(ep, HANDLE, unpollable, sync::POLL_IN, DATA), epoll::ERR_PERM);
    EXPECT_EQ(epoll::interest_count(ep), 0u);

    // A change asking for an unsupported mode is refused as well
    EXPECT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN, DATA), epoll::OK);
    EXPECT_EQ(epoll::modify_interest(ep, HANDLE, target, sync::POLL_IN | EDGE_TRIGGERED, DATA), epoll::ERR_INVAL);
    EXPECT_EQ(epoll::interest_count(ep), 1u);

    resource::resource_release(unpollable);
    resource::resource_release(target);
    resource::resource_release(other_ep);
    resource::resource_release(ep);
}

TEST(epoll_interests, an_epoll_holds_at_most_max_interests) {
    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_pollable_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);

    for (uint32_t i = 0; i < epoll::MAX_INTERESTS; i++) {
        ASSERT_EQ(epoll::add_interest(ep, static_cast<resource::handle_t>(i), target, sync::POLL_IN, DATA),
                  epoll::OK);
    }

    resource::handle_t next = static_cast<resource::handle_t>(epoll::MAX_INTERESTS);
    EXPECT_EQ(epoll::add_interest(ep, next, target, sync::POLL_IN, DATA), epoll::ERR_NOSPC);
    EXPECT_EQ(epoll::interest_count(ep), epoll::MAX_INTERESTS);

    resource::resource_release(target);
    resource::resource_release(ep);
}

TEST(epoll_interests, destroying_a_target_ends_its_interests) {
    resource::resource_object* ep = create_epoll();
    resource::resource_object* other_ep = create_epoll();
    resource::resource_object* target = create_target(&g_pollable_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(other_ep);
    ASSERT_NOT_NULL(target);

    EXPECT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN, DATA), epoll::OK);
    EXPECT_EQ(epoll::add_interest(ep, OTHER_HANDLE, target, sync::POLL_IN, DATA), epoll::OK);
    EXPECT_EQ(epoll::add_interest(other_ep, HANDLE, target, sync::POLL_IN, DATA), epoll::OK);

    resource::resource_release(target);

    EXPECT_EQ(epoll::interest_count(ep), 0u);
    EXPECT_EQ(epoll::interest_count(other_ep), 0u);

    resource::resource_release(other_ep);
    resource::resource_release(ep);
}

TEST(epoll_interests, destroying_an_epoll_ends_its_interests) {
    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_pollable_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);

    EXPECT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN, DATA), epoll::OK);
    EXPECT_EQ(epoll::add_interest(ep, OTHER_HANDLE, target, sync::POLL_IN, DATA), epoll::OK);
    EXPECT_EQ(target->watches.size(), 2u);

    resource::resource_release(ep);

    EXPECT_TRUE(target->watches.empty());

    resource::resource_release(target);
}

static destroy_race g_race;

// Both racers arrive before either releases its object for `round`
static void meet_other_racer(uint32_t round) {
    g_race.arrivals.fetch_add_acq_rel(1);
    (void)spin_wait_ge(g_race.arrivals, 2 * (round + 1));
}

static void release_race_epolls(void*) {
    for (uint32_t i = 0; i < RACE_ROUNDS; i++) {
        meet_other_racer(i);
        resource::resource_release(g_race.epolls[i]);
    }

    g_race.epolls_done.store_release(1);
    sched::exit(0);
}

static void release_race_targets(void*) {
    for (uint32_t i = 0; i < RACE_ROUNDS; i++) {
        meet_other_racer(i);
        resource::resource_release(g_race.targets[i]);
    }

    g_race.targets_done.store_release(1);
    sched::exit(0);
}

static bool start_racer(void (*entry)(void*), const char* name, uint32_t cpu) {
    sched::task* t = sched::create_kernel_task(entry, nullptr, name, sched::TASK_FLAG_ELEVATED);
    if (!t) {
        return false;
    }

    sched::enqueue_on(t, cpu);

    return true;
}

TEST(epoll_interests, destroying_an_epoll_and_its_target_together_is_safe) {
    if (smp::cpu_count() < RACE_CPUS) {
        return;
    }

    g_race.arrivals.store_relaxed(0);
    g_race.epolls_done.store_relaxed(0);
    g_race.targets_done.store_relaxed(0);

    for (uint32_t i = 0; i < RACE_ROUNDS; i++) {
        g_race.epolls[i] = create_epoll();
        g_race.targets[i] = create_target(&g_pollable_ops);
        ASSERT_NOT_NULL(g_race.epolls[i]);
        ASSERT_NOT_NULL(g_race.targets[i]);
        ASSERT_EQ(epoll::add_interest(g_race.epolls[i], HANDLE, g_race.targets[i], sync::POLL_IN, DATA), epoll::OK);
    }

    ASSERT_TRUE(start_racer(release_race_epolls, "epoll_racer", EPOLL_RACER_CPU));
    ASSERT_TRUE(start_racer(release_race_targets, "target_racer", TARGET_RACER_CPU));

    EXPECT_TRUE(spin_wait(g_race.epolls_done));
    EXPECT_TRUE(spin_wait(g_race.targets_done));
}
