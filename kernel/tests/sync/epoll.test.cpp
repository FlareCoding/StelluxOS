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
#include "clock/clock.h"

using test_helpers::spin_wait;
using test_helpers::spin_wait_ge;
using test_helpers::unpin;

TEST_SUITE(epoll_interests);
TEST_SUITE(epoll_wait);

constexpr resource::handle_t HANDLE         = 3;
constexpr resource::handle_t OTHER_HANDLE   = 4;
constexpr resource::handle_t THIRD_HANDLE   = 5;
constexpr uint64_t           DATA           = 0xC0FFEE;
constexpr uint64_t           OTHER_DATA     = 0xBEEF;
constexpr uint32_t           EXCLUSIVE      = 1u << 28;
constexpr uint32_t           WAKE_UP        = 1u << 29;
constexpr uint32_t           EDGE_TRIGGERED = 1u << 31;
constexpr uint32_t           RACE_ROUNDS    = 256;
constexpr uint32_t           WAIT_EVENTS    = 4;

constexpr int64_t MS               = 1000000;
constexpr int64_t NO_TIMEOUT       = -1;
constexpr int64_t SHORT_TIMEOUT_NS = 50 * MS;

// Passed by the time a wait first checks it
constexpr int64_t INSTANT_TIMEOUT_NS = 1;

// Long enough that both waiters are blocked before the wake, and it ends the wait of the one never woken
constexpr int64_t UNWOKEN_TIMEOUT_NS = 500 * MS;

// Under a spin_wait's limit, so a wait that is never woken still ends first
constexpr int64_t LONG_TIMEOUT_NS = 5000 * MS;

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

// One task keeps waking a target while another adds interests in it and ends them
struct wake_race {
    resource::resource_object* target;
    sync::atomic<uint32_t>     waker_started;
    sync::atomic<uint32_t>     adder_done;
    sync::atomic<uint32_t>     waker_done;
};

// Runs in a task of its own, since the test body runs on CPU 0's idle task, which must never block
struct wait_run {
    resource::resource_object* ep;
    int64_t                    timeout_ns;
    epoll::ready_event         events[WAIT_EVENTS];
    int32_t                    result;
    uint64_t                   elapsed_ns;
    sync::atomic<uint32_t>     done;
};

struct wait_race {
    resource::resource_object* ep;
    resource::resource_object* target;
    sync::atomic<uint32_t>     waiter_started;
    sync::atomic<uint32_t>     changer_done;
    sync::atomic<uint32_t>     waiter_done;
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

static bool start_racer(void (*entry)(void*), void* arg, const char* name, uint32_t cpu) {
    sched::task* t = sched::create_kernel_task(entry, arg, name, sched::TASK_FLAG_ELEVATED);
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

    ASSERT_TRUE(start_racer(release_race_epolls, nullptr, "epoll_racer", EPOLL_RACER_CPU));
    ASSERT_TRUE(start_racer(release_race_targets, nullptr, "target_racer", TARGET_RACER_CPU));

    EXPECT_TRUE(spin_wait(g_race.epolls_done));
    EXPECT_TRUE(spin_wait(g_race.targets_done));
}

// A target whose readiness the test sets, subscribed through one queue the test wakes
static sync::wait_queue g_probe_queue;
static uint32_t g_probe_events;

static uint32_t probe_poll(resource::resource_object*, sync::poll_table* pt) {
    if (pt) {
        sync::poll_subscribe(*pt, g_probe_queue);
    }

    return g_probe_events;
}

static const resource::resource_ops g_probe_ops = {
    .poll = probe_poll,
};

static void reset_probe(uint32_t events) {
    g_probe_queue.init();
    g_probe_events = events;
}

TEST(epoll_interests, an_interest_added_while_ready_is_queued) {
    reset_probe(sync::POLL_IN);
    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_probe_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);

    EXPECT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN, DATA), epoll::OK);
    EXPECT_EQ(epoll::ready_count(ep), 1u);

    // Readiness for events the interest did not ask for leaves it off the list, unless it is a hangup
    EXPECT_EQ(epoll::add_interest(ep, OTHER_HANDLE, target, sync::POLL_OUT, DATA), epoll::OK);
    EXPECT_EQ(epoll::ready_count(ep), 1u);

    g_probe_events = sync::POLL_HUP;
    EXPECT_EQ(epoll::remove_interest(ep, OTHER_HANDLE, target), epoll::OK);
    EXPECT_EQ(epoll::add_interest(ep, OTHER_HANDLE, target, sync::POLL_OUT, DATA), epoll::OK);
    EXPECT_EQ(epoll::ready_count(ep), 2u);

    resource::resource_release(target);
    resource::resource_release(ep);
}

TEST(epoll_interests, a_wake_queues_its_interest_once) {
    reset_probe(0);
    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_probe_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);

    EXPECT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN, DATA), epoll::OK);
    EXPECT_EQ(epoll::ready_count(ep), 0u);

    sync::wake_one(g_probe_queue);
    EXPECT_EQ(epoll::ready_count(ep), 1u);

    sync::wake_all(g_probe_queue);
    EXPECT_EQ(epoll::ready_count(ep), 1u);

    resource::resource_release(target);
    resource::resource_release(ep);
}

TEST(epoll_interests, a_modified_interest_is_queued_to_be_checked_again) {
    reset_probe(0);
    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_probe_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);

    EXPECT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN, DATA), epoll::OK);
    EXPECT_EQ(epoll::ready_count(ep), 0u);

    EXPECT_EQ(epoll::modify_interest(ep, HANDLE, target, sync::POLL_OUT, DATA), epoll::OK);
    EXPECT_EQ(epoll::ready_count(ep), 1u);

    resource::resource_release(target);
    resource::resource_release(ep);
}

TEST(epoll_interests, removing_an_interest_unsubscribes_and_unqueues_it) {
    reset_probe(0);
    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_probe_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);

    EXPECT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN, DATA), epoll::OK);
    EXPECT_FALSE(g_probe_queue.observers.empty());

    sync::wake_all(g_probe_queue);
    EXPECT_EQ(epoll::ready_count(ep), 1u);

    EXPECT_EQ(epoll::remove_interest(ep, HANDLE, target), epoll::OK);
    EXPECT_TRUE(g_probe_queue.observers.empty());
    EXPECT_EQ(epoll::ready_count(ep), 0u);

    sync::wake_all(g_probe_queue);
    EXPECT_EQ(epoll::ready_count(ep), 0u);

    resource::resource_release(target);
    resource::resource_release(ep);
}

TEST(epoll_interests, destroying_the_epoll_or_the_target_unsubscribes_its_interests) {
    reset_probe(0);
    resource::resource_object* ep = create_epoll();
    resource::resource_object* other_ep = create_epoll();
    resource::resource_object* target = create_target(&g_probe_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(other_ep);
    ASSERT_NOT_NULL(target);

    EXPECT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN, DATA), epoll::OK);
    EXPECT_EQ(epoll::add_interest(other_ep, HANDLE, target, sync::POLL_IN, DATA), epoll::OK);
    EXPECT_EQ(g_probe_queue.observers.size(), 2u);

    resource::resource_release(ep);
    EXPECT_EQ(g_probe_queue.observers.size(), 1u);

    sync::wake_all(g_probe_queue);
    EXPECT_EQ(epoll::ready_count(other_ep), 1u);

    resource::resource_release(target);
    EXPECT_TRUE(g_probe_queue.observers.empty());
    EXPECT_EQ(epoll::ready_count(other_ep), 0u);

    resource::resource_release(other_ep);
}

static wake_race g_wake_race;

static void add_and_end_interests(void*) {
    (void)spin_wait(g_wake_race.waker_started);

    for (uint32_t i = 0; i < RACE_ROUNDS; i++) {
        resource::resource_object* ep = create_epoll();
        if (!ep) {
            break;
        }

        (void)epoll::add_interest(ep, HANDLE, g_wake_race.target, sync::POLL_IN, DATA);

        // Alternates between ending the interest directly and through its epoll's destruction
        if (i % 2 == 0) {
            (void)epoll::remove_interest(ep, HANDLE, g_wake_race.target);
        }

        resource::resource_release(ep);
    }

    g_wake_race.adder_done.store_release(1);
    sched::exit(0);
}

static void wake_target_until_adder_is_done(void*) {
    g_wake_race.waker_started.store_release(1);

    while (!g_wake_race.adder_done.load_acquire()) {
        sync::wake_all(g_probe_queue);
    }

    g_wake_race.waker_done.store_release(1);
    sched::exit(0);
}

TEST(epoll_interests, a_wake_racing_the_end_of_its_interest_is_safe) {
    if (smp::cpu_count() < RACE_CPUS) {
        return;
    }

    reset_probe(0);
    g_wake_race.target = create_target(&g_probe_ops);
    ASSERT_NOT_NULL(g_wake_race.target);

    g_wake_race.waker_started.store_relaxed(0);
    g_wake_race.adder_done.store_relaxed(0);
    g_wake_race.waker_done.store_relaxed(0);

    // The adder starts first, so a waker that fails to start leaves nothing spinning forever
    ASSERT_TRUE(start_racer(add_and_end_interests, nullptr, "interest_adder", EPOLL_RACER_CPU));
    ASSERT_TRUE(start_racer(wake_target_until_adder_is_done, nullptr, "target_waker", TARGET_RACER_CPU));

    EXPECT_TRUE(spin_wait(g_wake_race.adder_done));
    EXPECT_TRUE(spin_wait(g_wake_race.waker_done));
    EXPECT_TRUE(g_probe_queue.observers.empty());

    resource::resource_release(g_wake_race.target);
}

static resource::resource_object* g_adding_epoll;
static uint32_t                   g_queued_while_adding;

static uint32_t wake_while_subscribing(resource::resource_object*, sync::poll_table* pt) {
    if (pt) {
        sync::poll_subscribe(*pt, g_probe_queue);
        sync::wake_all(g_probe_queue);
        g_queued_while_adding = epoll::ready_count(g_adding_epoll);
    }

    return 0;
}

static const resource::resource_ops g_waking_ops = {
    .poll = wake_while_subscribing,
};

TEST(epoll_wait, a_wake_while_an_interest_is_added_counts_once_the_add_succeeds) {
    reset_probe(0);
    g_adding_epoll = create_epoll();
    resource::resource_object* target = create_target(&g_waking_ops);
    ASSERT_NOT_NULL(g_adding_epoll);
    ASSERT_NOT_NULL(target);

    EXPECT_EQ(epoll::add_interest(g_adding_epoll, HANDLE, target, sync::POLL_IN, DATA), epoll::OK);
    EXPECT_EQ(g_queued_while_adding, 0u);
    EXPECT_EQ(epoll::ready_count(g_adding_epoll), 1u);

    // Emptied by the wait, so only the refused add's wake can queue
    epoll::ready_event events[WAIT_EVENTS] = {};
    EXPECT_EQ(epoll::wait(g_adding_epoll, events, WAIT_EVENTS, 0), 0);
    EXPECT_EQ(epoll::add_interest(g_adding_epoll, HANDLE, target, sync::POLL_IN, OTHER_DATA), epoll::ERR_EXIST);
    EXPECT_EQ(g_queued_while_adding, 1u);
    EXPECT_EQ(epoll::ready_count(g_adding_epoll), 1u);

    resource::resource_release(target);
    resource::resource_release(g_adding_epoll);
}

TEST(epoll_wait, invalid_waits_are_refused) {
    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_pollable_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);

    epoll::ready_event events[WAIT_EVENTS] = {};
    EXPECT_EQ(epoll::wait(target, events, WAIT_EVENTS, 0), epoll::ERR_INVAL);
    EXPECT_EQ(epoll::wait(ep, nullptr, WAIT_EVENTS, 0), epoll::ERR_INVAL);
    EXPECT_EQ(epoll::wait(ep, events, 0, 0), epoll::ERR_INVAL);

    resource::resource_release(target);
    resource::resource_release(ep);
}

TEST(epoll_wait, a_wait_with_a_zero_timeout_never_blocks) {
    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_pollable_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);

    epoll::ready_event events[WAIT_EVENTS] = {};
    EXPECT_EQ(epoll::wait(ep, events, WAIT_EVENTS, 0), 0);

    EXPECT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN, DATA), epoll::OK);
    EXPECT_EQ(epoll::wait(ep, events, WAIT_EVENTS, 0), 0);

    resource::resource_release(target);
    resource::resource_release(ep);
}

TEST(epoll_wait, a_wait_reports_ready_interests_with_their_data) {
    reset_probe(sync::POLL_IN | sync::POLL_OUT);
    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_probe_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);

    ASSERT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN, DATA), epoll::OK);
    ASSERT_EQ(epoll::add_interest(ep, OTHER_HANDLE, target, sync::POLL_OUT | sync::POLL_RDHUP, OTHER_DATA), epoll::OK);

    epoll::ready_event events[WAIT_EVENTS] = {};
    ASSERT_EQ(epoll::wait(ep, events, WAIT_EVENTS, 0), 2);
    EXPECT_EQ(events[0].events, sync::POLL_IN);
    EXPECT_EQ(events[0].data, DATA);
    EXPECT_EQ(events[1].events, sync::POLL_OUT);
    EXPECT_EQ(events[1].data, OTHER_DATA);

    resource::resource_release(target);
    resource::resource_release(ep);
}

TEST(epoll_wait, a_hangup_is_reported_whether_or_not_it_was_asked_for) {
    reset_probe(sync::POLL_HUP | sync::POLL_OUT);
    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_probe_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);

    ASSERT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN, DATA), epoll::OK);

    epoll::ready_event events[WAIT_EVENTS] = {};
    ASSERT_EQ(epoll::wait(ep, events, WAIT_EVENTS, 0), 1);
    EXPECT_EQ(events[0].events, sync::POLL_HUP);

    resource::resource_release(target);
    resource::resource_release(ep);
}

TEST(epoll_wait, a_ready_interest_is_reported_by_every_wait_until_it_is_not_ready) {
    reset_probe(sync::POLL_IN);
    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_probe_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);

    ASSERT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN, DATA), epoll::OK);

    epoll::ready_event events[WAIT_EVENTS] = {};
    EXPECT_EQ(epoll::wait(ep, events, WAIT_EVENTS, 0), 1);
    EXPECT_EQ(epoll::wait(ep, events, WAIT_EVENTS, 0), 1);

    g_probe_events = 0;
    EXPECT_EQ(epoll::wait(ep, events, WAIT_EVENTS, 0), 0);
    EXPECT_EQ(epoll::ready_count(ep), 0u);

    resource::resource_release(target);
    resource::resource_release(ep);
}

TEST(epoll_wait, a_one_shot_interest_is_reported_once_until_it_is_changed) {
    reset_probe(sync::POLL_IN);
    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_probe_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);

    ASSERT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN | epoll::ONE_SHOT, DATA), epoll::OK);

    epoll::ready_event events[WAIT_EVENTS] = {};
    EXPECT_EQ(epoll::wait(ep, events, WAIT_EVENTS, 0), 1);
    EXPECT_EQ(events[0].events, sync::POLL_IN);

    sync::wake_all(g_probe_queue);
    EXPECT_EQ(epoll::ready_count(ep), 0u);
    EXPECT_EQ(epoll::wait(ep, events, WAIT_EVENTS, 0), 0);

    EXPECT_EQ(epoll::modify_interest(ep, HANDLE, target, sync::POLL_IN | epoll::ONE_SHOT, OTHER_DATA), epoll::OK);
    EXPECT_EQ(epoll::wait(ep, events, WAIT_EVENTS, 0), 1);
    EXPECT_EQ(events[0].data, OTHER_DATA);
    EXPECT_EQ(epoll::wait(ep, events, WAIT_EVENTS, 0), 0);

    resource::resource_release(target);
    resource::resource_release(ep);
}

TEST(epoll_wait, a_disarmed_one_shot_interest_reports_no_hangup) {
    reset_probe(sync::POLL_HUP);
    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_probe_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);

    ASSERT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN | epoll::ONE_SHOT, DATA), epoll::OK);

    epoll::ready_event events[WAIT_EVENTS] = {};
    EXPECT_EQ(epoll::wait(ep, events, WAIT_EVENTS, 0), 1);
    EXPECT_EQ(events[0].events, sync::POLL_HUP);
    EXPECT_EQ(epoll::wait(ep, events, WAIT_EVENTS, 0), 0);

    resource::resource_release(target);
    resource::resource_release(ep);
}

TEST(epoll_wait, a_wait_reports_at_most_max_events_and_ready_interests_take_turns) {
    reset_probe(sync::POLL_IN);
    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_probe_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);

    ASSERT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN, static_cast<uint64_t>(HANDLE)), epoll::OK);
    ASSERT_EQ(epoll::add_interest(ep, OTHER_HANDLE, target, sync::POLL_IN, static_cast<uint64_t>(OTHER_HANDLE)),
              epoll::OK);
    ASSERT_EQ(epoll::add_interest(ep, THIRD_HANDLE, target, sync::POLL_IN, static_cast<uint64_t>(THIRD_HANDLE)),
              epoll::OK);

    epoll::ready_event events[WAIT_EVENTS] = {};
    ASSERT_EQ(epoll::wait(ep, events, 2, 0), 2);
    EXPECT_EQ(events[0].data, static_cast<uint64_t>(HANDLE));
    EXPECT_EQ(events[1].data, static_cast<uint64_t>(OTHER_HANDLE));

    ASSERT_EQ(epoll::wait(ep, events, 2, 0), 2);
    EXPECT_EQ(events[0].data, static_cast<uint64_t>(THIRD_HANDLE));
    EXPECT_EQ(events[1].data, static_cast<uint64_t>(HANDLE));

    resource::resource_release(target);
    resource::resource_release(ep);
}

TEST(epoll_wait, a_wait_reports_at_most_max_wait_events) {
    reset_probe(sync::POLL_IN);
    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_probe_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);

    for (uint32_t i = 0; i <= epoll::MAX_WAIT_EVENTS; i++) {
        ASSERT_EQ(epoll::add_interest(ep, static_cast<resource::handle_t>(i), target, sync::POLL_IN, DATA), epoll::OK);
    }

    epoll::ready_event events[epoll::MAX_WAIT_EVENTS + 1] = {};
    EXPECT_EQ(epoll::wait(ep, events, epoll::MAX_WAIT_EVENTS + 1, 0), static_cast<int32_t>(epoll::MAX_WAIT_EVENTS));

    resource::resource_release(target);
    resource::resource_release(ep);
}

TEST(epoll_wait, a_wait_looks_past_queued_interests_that_are_no_longer_ready) {
    reset_probe(sync::POLL_IN);
    resource::resource_object* ep = create_epoll();
    resource::resource_object* idle = create_target(&g_pollable_ops);
    resource::resource_object* ready = create_target(&g_probe_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(idle);
    ASSERT_NOT_NULL(ready);

    // A change queues an interest unchecked, filling a batch ahead of the ready one
    for (uint32_t i = 0; i < epoll::MAX_WAIT_EVENTS; i++) {
        auto handle = static_cast<resource::handle_t>(i);
        ASSERT_EQ(epoll::add_interest(ep, handle, idle, sync::POLL_IN, DATA), epoll::OK);
        ASSERT_EQ(epoll::modify_interest(ep, handle, idle, sync::POLL_IN, DATA), epoll::OK);
    }

    ASSERT_EQ(epoll::add_interest(ep, HANDLE, ready, sync::POLL_IN, OTHER_DATA), epoll::OK);

    epoll::ready_event events[WAIT_EVENTS] = {};
    ASSERT_EQ(epoll::wait(ep, events, WAIT_EVENTS, 0), 1);
    EXPECT_EQ(events[0].data, OTHER_DATA);
    EXPECT_EQ(epoll::ready_count(ep), 1u);

    resource::resource_release(ready);
    resource::resource_release(idle);
    resource::resource_release(ep);
}

static wait_run g_wait;
static wait_run g_other_wait;

static void run_wait(void* arg) {
    wait_run& run = *static_cast<wait_run*>(arg);

    uint64_t start = clock::now_ns();
    run.result = epoll::wait(run.ep, run.events, WAIT_EVENTS, run.timeout_ns);
    run.elapsed_ns = clock::now_ns() - start;

    run.done.store_release(1);
    sched::exit(0);
}

static void prepare_wait(wait_run& run, resource::resource_object* ep, int64_t timeout_ns) {
    run.ep = ep;
    run.timeout_ns = timeout_ns;
    run.result = 0;
    run.elapsed_ns = 0;
    run.done.store_relaxed(0);
}

static sched::task* start_wait(wait_run& run, resource::resource_object* ep, int64_t timeout_ns) {
    prepare_wait(run, ep, timeout_ns);

    return test_helpers::start_pinned_task(run_wait, &run, "epoll_wait");
}

TEST(epoll_wait, a_wait_times_out_when_nothing_becomes_ready) {
    reset_probe(0);
    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_probe_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);

    ASSERT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN, DATA), epoll::OK);

    sched::task* t = start_wait(g_wait, ep, SHORT_TIMEOUT_NS);
    ASSERT_NOT_NULL(t);
    EXPECT_TRUE(spin_wait(g_wait.done));
    unpin(t);

    EXPECT_EQ(g_wait.result, 0);
    EXPECT_GE(g_wait.elapsed_ns, static_cast<uint64_t>(SHORT_TIMEOUT_NS));

    resource::resource_release(target);
    resource::resource_release(ep);
}

TEST(epoll_wait, a_blocked_wait_is_woken_when_an_interest_becomes_ready) {
    reset_probe(0);
    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_probe_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);

    ASSERT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN, DATA), epoll::OK);

    sched::task* t = start_wait(g_wait, ep, LONG_TIMEOUT_NS);
    ASSERT_NOT_NULL(t);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(t));

    g_probe_events = sync::POLL_IN;
    sync::wake_all(g_probe_queue);

    EXPECT_TRUE(spin_wait(g_wait.done));
    unpin(t);

    EXPECT_EQ(g_wait.result, 1);
    EXPECT_EQ(g_wait.events[0].data, DATA);
    EXPECT_LT(g_wait.elapsed_ns, static_cast<uint64_t>(LONG_TIMEOUT_NS));

    resource::resource_release(target);
    resource::resource_release(ep);
}

TEST(epoll_wait, adding_a_ready_interest_wakes_a_blocked_wait) {
    reset_probe(sync::POLL_IN);
    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_probe_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);

    sched::task* t = start_wait(g_wait, ep, LONG_TIMEOUT_NS);
    ASSERT_NOT_NULL(t);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(t));

    ASSERT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN, DATA), epoll::OK);

    EXPECT_TRUE(spin_wait(g_wait.done));
    unpin(t);

    EXPECT_EQ(g_wait.result, 1);
    EXPECT_LT(g_wait.elapsed_ns, static_cast<uint64_t>(LONG_TIMEOUT_NS));

    resource::resource_release(target);
    resource::resource_release(ep);
}

TEST(epoll_wait, changing_an_interest_to_ready_events_wakes_a_blocked_wait) {
    reset_probe(sync::POLL_OUT);
    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_probe_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);

    ASSERT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN, DATA), epoll::OK);

    sched::task* t = start_wait(g_wait, ep, LONG_TIMEOUT_NS);
    ASSERT_NOT_NULL(t);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(t));

    ASSERT_EQ(epoll::modify_interest(ep, HANDLE, target, sync::POLL_OUT, DATA), epoll::OK);

    EXPECT_TRUE(spin_wait(g_wait.done));
    unpin(t);

    EXPECT_EQ(g_wait.result, 1);
    EXPECT_EQ(g_wait.events[0].events, sync::POLL_OUT);
    EXPECT_LT(g_wait.elapsed_ns, static_cast<uint64_t>(LONG_TIMEOUT_NS));

    resource::resource_release(target);
    resource::resource_release(ep);
}

TEST(epoll_wait, a_wake_is_passed_on_to_every_waiter_while_the_interest_stays_ready) {
    reset_probe(0);
    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_probe_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);

    ASSERT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN, DATA), epoll::OK);

    sched::task* first = start_wait(g_wait, ep, LONG_TIMEOUT_NS);
    sched::task* second = start_wait(g_other_wait, ep, LONG_TIMEOUT_NS);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(first));
    ASSERT_TRUE(test_helpers::blocks_before_deadline(second));

    g_probe_events = sync::POLL_IN;
    sync::wake_one(g_probe_queue);

    EXPECT_TRUE(spin_wait(g_wait.done));
    EXPECT_TRUE(spin_wait(g_other_wait.done));
    unpin(first);
    unpin(second);

    EXPECT_EQ(g_wait.result, 1);
    EXPECT_EQ(g_other_wait.result, 1);
    EXPECT_LT(g_wait.elapsed_ns, static_cast<uint64_t>(LONG_TIMEOUT_NS));
    EXPECT_LT(g_other_wait.elapsed_ns, static_cast<uint64_t>(LONG_TIMEOUT_NS));

    resource::resource_release(target);
    resource::resource_release(ep);
}

TEST(epoll_wait, a_one_shot_wake_reaches_a_single_waiter) {
    reset_probe(0);
    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_probe_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);

    ASSERT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN | epoll::ONE_SHOT, DATA), epoll::OK);

    sched::task* first = start_wait(g_wait, ep, UNWOKEN_TIMEOUT_NS);
    sched::task* second = start_wait(g_other_wait, ep, UNWOKEN_TIMEOUT_NS);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(first));
    ASSERT_TRUE(test_helpers::blocks_before_deadline(second));

    g_probe_events = sync::POLL_IN;
    sync::wake_all(g_probe_queue);

    EXPECT_TRUE(spin_wait(g_wait.done));
    EXPECT_TRUE(spin_wait(g_other_wait.done));
    unpin(first);
    unpin(second);

    EXPECT_EQ(g_wait.result + g_other_wait.result, 1);

    resource::resource_release(target);
    resource::resource_release(ep);
}

TEST(epoll_wait, a_kill_interrupts_a_blocked_wait) {
    reset_probe(0);
    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_probe_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);

    ASSERT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN, DATA), epoll::OK);

    sched::task* t = start_wait(g_wait, ep, NO_TIMEOUT);
    ASSERT_NOT_NULL(t);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(t));

    sched::force_wake_for_kill(t);

    EXPECT_TRUE(spin_wait(g_wait.done));
    unpin(t);

    EXPECT_EQ(g_wait.result, epoll::ERR_INTR);

    resource::resource_release(target);
    resource::resource_release(ep);
}

static int32_t                g_zero_timeout_result;
static int32_t                g_passed_timeout_result;
static sync::atomic<uint32_t> g_killed_waits_done;

static void wait_with_a_kill_pending(void* arg) {
    auto* ep = static_cast<resource::resource_object*>(arg);
    epoll::ready_event events[WAIT_EVENTS] = {};
    sched::force_wake_for_kill(sched::current());

    g_zero_timeout_result = epoll::wait(ep, events, WAIT_EVENTS, 0);
    g_passed_timeout_result = epoll::wait(ep, events, WAIT_EVENTS, INSTANT_TIMEOUT_NS);

    g_killed_waits_done.store_release(1);
    sched::exit(0);
}

TEST(epoll_wait, a_pending_signal_outranks_a_passed_timeout_but_not_a_zero_one) {
    resource::resource_object* ep = create_epoll();
    ASSERT_NOT_NULL(ep);

    g_killed_waits_done.store_relaxed(0);
    sched::task* t = test_helpers::start_pinned_task(wait_with_a_kill_pending, ep, "killed_wait");
    ASSERT_NOT_NULL(t);
    EXPECT_TRUE(spin_wait(g_killed_waits_done));
    unpin(t);

    EXPECT_EQ(g_zero_timeout_result, 0);
    EXPECT_EQ(g_passed_timeout_result, epoll::ERR_INTR);

    resource::resource_release(ep);
}

static sync::atomic<uint32_t> g_check_started;
static sync::atomic<uint32_t> g_check_released;
static sync::atomic<uint32_t> g_held_closes;
static wait_run               g_held_wait;

// A wait checks without a table, and its check holds until the test releases it
static uint32_t hold_check(resource::resource_object*, sync::poll_table* pt) {
    if (!pt) {
        g_check_started.store_release(1);
        (void)spin_wait(g_check_released);
    }

    return sync::POLL_IN;
}

static void count_close(resource::resource_object*) {
    g_held_closes.fetch_add_acq_rel(1);
}

static const resource::resource_ops g_held_ops = {
    .close = count_close,
    .poll  = hold_check,
};

// Off CPU 0, since the waiter spins inside the check
static bool start_held_wait(resource::resource_object* ep) {
    g_check_started.store_relaxed(0);
    g_check_released.store_relaxed(0);
    prepare_wait(g_held_wait, ep, 0);

    return start_racer(run_wait, &g_held_wait, "held_wait", EPOLL_RACER_CPU);
}

TEST(epoll_wait, an_interest_removed_while_a_wait_checks_it_is_not_reported) {
    if (smp::cpu_count() < RACE_CPUS) {
        return;
    }

    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_held_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);

    ASSERT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN, DATA), epoll::OK);
    ASSERT_TRUE(start_held_wait(ep));
    ASSERT_TRUE(spin_wait(g_check_started));

    EXPECT_EQ(epoll::remove_interest(ep, HANDLE, target), epoll::OK);
    g_check_released.store_release(1);

    ASSERT_TRUE(spin_wait(g_held_wait.done));
    EXPECT_EQ(g_held_wait.result, 0);
    EXPECT_EQ(epoll::ready_count(ep), 0u);

    resource::resource_release(target);
    resource::resource_release(ep);
}

TEST(epoll_wait, a_target_released_while_a_wait_checks_it_lives_until_the_check_ends) {
    if (smp::cpu_count() < RACE_CPUS) {
        return;
    }

    g_held_closes.store_relaxed(0);
    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_held_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);

    ASSERT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN, DATA), epoll::OK);
    ASSERT_TRUE(start_held_wait(ep));
    ASSERT_TRUE(spin_wait(g_check_started));

    resource::resource_release(target);
    EXPECT_EQ(g_held_closes.load_acquire(), 0u);
    EXPECT_EQ(epoll::interest_count(ep), 1u);
    g_check_released.store_release(1);

    ASSERT_TRUE(spin_wait(g_held_wait.done));
    EXPECT_EQ(g_held_wait.result, 1);
    EXPECT_EQ(g_held_closes.load_acquire(), 1u);
    EXPECT_EQ(epoll::interest_count(ep), 0u);
    EXPECT_EQ(epoll::ready_count(ep), 0u);

    resource::resource_release(ep);
}

TEST(epoll_wait, a_one_shot_interest_changed_while_a_wait_checks_it_is_not_left_queued) {
    if (smp::cpu_count() < RACE_CPUS) {
        return;
    }

    resource::resource_object* ep = create_epoll();
    resource::resource_object* target = create_target(&g_held_ops);
    ASSERT_NOT_NULL(ep);
    ASSERT_NOT_NULL(target);

    ASSERT_EQ(epoll::add_interest(ep, HANDLE, target, sync::POLL_IN | epoll::ONE_SHOT, DATA), epoll::OK);
    ASSERT_TRUE(start_held_wait(ep));
    ASSERT_TRUE(spin_wait(g_check_started));

    EXPECT_EQ(epoll::modify_interest(ep, HANDLE, target, sync::POLL_IN | epoll::ONE_SHOT, OTHER_DATA), epoll::OK);
    EXPECT_EQ(epoll::ready_count(ep), 1u);
    g_check_released.store_release(1);

    ASSERT_TRUE(spin_wait(g_held_wait.done));
    EXPECT_EQ(g_held_wait.result, 1);
    EXPECT_EQ(g_held_wait.events[0].data, OTHER_DATA);
    EXPECT_EQ(epoll::ready_count(ep), 0u);

    resource::resource_release(target);
    resource::resource_release(ep);
}

static wait_race g_wait_race;

static void change_interests(void*) {
    (void)spin_wait(g_wait_race.waiter_started);

    for (uint32_t i = 0; i < RACE_ROUNDS; i++) {
        (void)epoll::add_interest(g_wait_race.ep, HANDLE, g_wait_race.target, sync::POLL_IN, DATA);
        (void)epoll::modify_interest(g_wait_race.ep, HANDLE, g_wait_race.target, sync::POLL_IN, OTHER_DATA);
        (void)epoll::remove_interest(g_wait_race.ep, HANDLE, g_wait_race.target);
    }

    g_wait_race.changer_done.store_release(1);
    sched::exit(0);
}

static void wake_and_wait_until_changer_is_done(void*) {
    epoll::ready_event events[WAIT_EVENTS] = {};
    g_wait_race.waiter_started.store_release(1);

    while (!g_wait_race.changer_done.load_acquire()) {
        sync::wake_all(g_probe_queue);
        (void)epoll::wait(g_wait_race.ep, events, WAIT_EVENTS, 0);
    }

    g_wait_race.waiter_done.store_release(1);
    sched::exit(0);
}

TEST(epoll_wait, waits_racing_the_end_of_their_interests_are_safe) {
    if (smp::cpu_count() < RACE_CPUS) {
        return;
    }

    reset_probe(sync::POLL_IN);
    g_wait_race.ep = create_epoll();
    g_wait_race.target = create_target(&g_probe_ops);
    ASSERT_NOT_NULL(g_wait_race.ep);
    ASSERT_NOT_NULL(g_wait_race.target);

    g_wait_race.waiter_started.store_relaxed(0);
    g_wait_race.changer_done.store_relaxed(0);
    g_wait_race.waiter_done.store_relaxed(0);

    // The changer starts first, so a waiter that fails to start leaves nothing spinning forever
    ASSERT_TRUE(start_racer(change_interests, nullptr, "interest_changer", EPOLL_RACER_CPU));
    ASSERT_TRUE(start_racer(wake_and_wait_until_changer_is_done, nullptr, "epoll_waiter", TARGET_RACER_CPU));

    EXPECT_TRUE(spin_wait(g_wait_race.changer_done));
    EXPECT_TRUE(spin_wait(g_wait_race.waiter_done));
    EXPECT_EQ(epoll::interest_count(g_wait_race.ep), 0u);
    EXPECT_EQ(epoll::ready_count(g_wait_race.ep), 0u);
    EXPECT_TRUE(g_probe_queue.observers.empty());

    resource::resource_release(g_wait_race.target);
    resource::resource_release(g_wait_race.ep);
}
