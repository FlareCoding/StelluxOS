#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_proc.h"
#include "resource/resource.h"
#include "resource/handle_batch.h"
#include "resource/socket_ops.h"
#include "resource/providers/proc_provider.h"
#include "socket/unix_socket.h"
#include "net/inet.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sync/atomic.h"
#include "mm/heap.h"

TEST_SUITE(proc_handles_syscall);

static const char PROGRAM[] = "/bin/hello";
constexpr uint64_t HELD_SLOT = 3;

// A handle in `task`'s table to `obj`, or -1
static resource::handle_t handle_to(sched::task* task, resource::resource_object* obj) {
    resource::handle_t handle = -1;
    if (resource::alloc_task_handle(task, obj, obj->type, 0, &handle) != resource::HANDLE_OK) {
        return -1;
    }

    return handle;
}

static int64_t set_handle(uint64_t process, uint64_t slot, uint64_t held) {
    return sys_proc_set_handle(process, slot, held, 0, 0, 0);
}

// Sends one byte carrying `passenger` on `obj` without waiting, letting the batch go when the send did not take it
static ssize_t send_passenger(resource::resource_object* obj, resource::resource_object* passenger) {
    resource::handle_batch* batch = resource::create_handle_batch(1);
    if (!batch) {
        return resource::ERR_NOMEM;
    }

    resource::resource_add_ref(passenger);
    batch->entries[0] = {passenger, passenger->type, resource::RIGHT_READ};

    ssize_t n = obj->ops->socket->sendmsg(obj, "x", 1, net::inet::MSG_DONTWAIT, nullptr, 0, batch);
    if (n <= 0) {
        resource::handle_batch_release(batch);
    }

    return n;
}

TEST(proc_handles_syscall, installing_a_process_into_its_own_table_reports_eloop) {
    sched::task* task = sched::current();
    resource::resource_object* process = test_helpers::create_unstarted_process_object(PROGRAM);
    ASSERT_NOT_NULL(process);
    resource::handle_t handle = handle_to(task, process);
    ASSERT_TRUE(handle >= 0);

    EXPECT_EQ(set_handle(handle, HELD_SLOT, handle), syscall::ELOOP);

    EXPECT_EQ(resource::close(task, handle), resource::OK);
    EXPECT_EQ(process->ref_count(), 1u);
    resource::resource_release(process);
}

TEST(proc_handles_syscall, installing_a_process_into_one_it_holds_reports_eloop) {
    sched::task* task = sched::current();
    resource::resource_object* first = test_helpers::create_unstarted_process_object(PROGRAM);
    resource::resource_object* second = test_helpers::create_unstarted_process_object(PROGRAM);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);
    resource::handle_t first_handle = handle_to(task, first);
    resource::handle_t second_handle = handle_to(task, second);
    ASSERT_TRUE(first_handle >= 0 && second_handle >= 0);

    EXPECT_EQ(set_handle(first_handle, HELD_SLOT, second_handle), 0);
    EXPECT_EQ(set_handle(second_handle, HELD_SLOT, first_handle), syscall::ELOOP);

    EXPECT_EQ(resource::close(task, first_handle), resource::OK);
    EXPECT_EQ(resource::close(task, second_handle), resource::OK);
    resource::resource_release(second);
    resource::resource_release(first);
}

TEST(proc_handles_syscall, installing_a_socket_whose_queue_holds_the_process_reports_eloop) {
    sched::task* task = sched::current();
    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);
    resource::resource_object* process = test_helpers::create_unstarted_process_object(PROGRAM);
    ASSERT_NOT_NULL(process);
    resource::handle_t process_handle = handle_to(task, process);
    resource::handle_t reader_handle = handle_to(task, obj_b);
    ASSERT_TRUE(process_handle >= 0 && reader_handle >= 0);

    // The process is queued for the socket to read, so the socket may not be handed to the process
    ASSERT_EQ(send_passenger(obj_a, process), static_cast<ssize_t>(1));
    EXPECT_EQ(set_handle(process_handle, HELD_SLOT, reader_handle), syscall::ELOOP);

    EXPECT_EQ(resource::close(task, process_handle), resource::OK);
    EXPECT_EQ(resource::close(task, reader_handle), resource::OK);
    resource::resource_release(process);
    resource::resource_release(obj_b);
    EXPECT_EQ(obj_a->ref_count(), 1u);
    resource::resource_release(obj_a);
}

// Two tasks each installing one process into the other, where only the first to take the lock may succeed
constexpr uint32_t RACING_ROUNDS = 32;

struct racing_install_run {
    resource::handle_t process;
    resource::handle_t held;
    int64_t result;
    sync::atomic<uint32_t> done;
};

static racing_install_run g_racing_installs[2];

static void run_racing_install(void* arg) {
    auto& run = *static_cast<racing_install_run*>(arg);
    run.result = set_handle(static_cast<uint64_t>(run.process), HELD_SLOT, static_cast<uint64_t>(run.held));
    run.done.store_release(1);
    sched::exit(0);
}

// A task that runs `run` once its table holds handles to both processes
static sched::task* start_racing_install(racing_install_run& run, resource::resource_object* process,
                                         resource::resource_object* held) {
    run.result = 0;
    run.done.store_relaxed(0);

    sched::task* t = sched::create_kernel_task(run_racing_install, &run, "racing_install", sched::TASK_FLAG_ELEVATED);
    if (!t) {
        return nullptr;
    }

    t->add_ref();
    run.process = handle_to(t, process);
    run.held = handle_to(t, held);
    sched::enqueue(t);

    return t;
}

TEST(proc_handles_syscall, racing_installs_that_would_form_a_loop_refuse_exactly_one) {
    for (uint32_t round = 0; round < RACING_ROUNDS; round++) {
        resource::resource_object* first = test_helpers::create_unstarted_process_object(PROGRAM);
        resource::resource_object* second = test_helpers::create_unstarted_process_object(PROGRAM);
        ASSERT_NOT_NULL(first);
        ASSERT_NOT_NULL(second);

        sched::task* into_first = start_racing_install(g_racing_installs[0], first, second);
        sched::task* into_second = start_racing_install(g_racing_installs[1], second, first);
        ASSERT_NOT_NULL(into_first);
        ASSERT_NOT_NULL(into_second);
        EXPECT_TRUE(test_helpers::spin_wait(g_racing_installs[0].done));
        EXPECT_TRUE(test_helpers::spin_wait(g_racing_installs[1].done));

        int64_t results[2] = {g_racing_installs[0].result, g_racing_installs[1].result};
        EXPECT_TRUE((results[0] == 0 && results[1] == syscall::ELOOP) ||
                    (results[0] == syscall::ELOOP && results[1] == 0));

        test_helpers::unpin(into_first);
        test_helpers::unpin(into_second);
        resource::resource_release(second);
        resource::resource_release(first);
    }
}

// Random sends of processes and sockets over sockets, installs into processes, and receives, after which
// closing everything the test holds must free every process and socket
constexpr uint32_t STRESS_PROCESSES = 12;
constexpr uint32_t STRESS_SOCKETS = 16;
constexpr uint32_t STRESS_SLOTS = 8;
constexpr uint32_t STRESS_ROUNDS = 400;
constexpr uint64_t SENTINEL_SLOT = STRESS_SLOTS;

static resource::resource_object* g_stress_processes[STRESS_PROCESSES];
static resource::handle_t g_stress_process_handles[STRESS_PROCESSES];
static resource::resource_object* g_stress_sockets[STRESS_SOCKETS];
static resource::handle_t g_stress_socket_handles[STRESS_SOCKETS];

// Closed exactly when the process or socket that held it is freed
static uint32_t g_sentinel_closes;

static void close_sentinel(resource::resource_object*) {
    g_sentinel_closes++;
}

static const resource::resource_ops g_sentinel_ops = {
    .close = close_sentinel,
};

static resource::resource_object* make_sentinel() {
    auto* obj = heap::kalloc_new<resource::resource_object>();
    if (obj) {
        obj->type = resource::resource_type::FILE;
        obj->ops = &g_sentinel_ops;
    }

    return obj;
}

// A fixed sequence, so a failing run repeats
static uint32_t g_stress_state;

static uint32_t stress_pick(uint32_t bound) {
    g_stress_state = g_stress_state * 1664525u + 1013904223u;

    return (g_stress_state >> 8) % bound;
}

// Gives each process a sentinel in its table and queues one on each socket
static bool plant_sentinels() {
    for (uint32_t i = 0; i < STRESS_PROCESSES; i++) {
        resource::resource_object* sentinel = make_sentinel();
        auto* pr = resource::proc_provider::get_proc_resource(g_stress_processes[i]);
        if (!sentinel || !pr) {
            return false;
        }

        int32_t rc = resource::install_handle_at(pr->child->handles, SENTINEL_SLOT, sentinel, sentinel->type, 0);
        resource::resource_release(sentinel);
        if (rc != resource::HANDLE_OK) {
            return false;
        }
    }

    for (uint32_t i = 0; i < STRESS_SOCKETS; i++) {
        resource::resource_object* sentinel = make_sentinel();
        if (!sentinel) {
            return false;
        }

        ssize_t n = send_passenger(g_stress_sockets[i ^ 1], sentinel);
        resource::resource_release(sentinel);
        if (n != 1) {
            return false;
        }
    }

    return true;
}

static void stress_round() {
    uint32_t kind = stress_pick(3);

    if (kind == 0) {
        resource::handle_t process = g_stress_process_handles[stress_pick(STRESS_PROCESSES)];
        resource::handle_t held = stress_pick(2) == 0 ? g_stress_process_handles[stress_pick(STRESS_PROCESSES)]
                                                      : g_stress_socket_handles[stress_pick(STRESS_SOCKETS)];
        int64_t rc = set_handle(static_cast<uint64_t>(process), stress_pick(STRESS_SLOTS), static_cast<uint64_t>(held));
        EXPECT_TRUE(rc == 0 || rc == syscall::ELOOP);
        return;
    }

    resource::resource_object* sock = g_stress_sockets[stress_pick(STRESS_SOCKETS)];
    if (kind == 1) {
        resource::resource_object* passenger = stress_pick(2) == 0 ? g_stress_processes[stress_pick(STRESS_PROCESSES)]
                                                                   : g_stress_sockets[stress_pick(STRESS_SOCKETS)];
        ssize_t n = send_passenger(sock, passenger);
        EXPECT_TRUE(n == 1 || n == resource::ERR_LOOP || n == resource::ERR_AGAIN);
        return;
    }

    char byte = 0;
    resource::handle_batch* batch = nullptr;
    ssize_t n = sock->ops->socket->recvmsg(sock, &byte, 1, net::inet::MSG_DONTWAIT, nullptr, nullptr, &batch);
    EXPECT_TRUE(n == 1 || n == resource::ERR_AGAIN);
    resource::handle_batch_release(batch);
}

TEST(proc_handles_syscall, random_sends_and_installs_leave_nothing_alive_once_everything_closes) {
    sched::task* task = sched::current();
    g_stress_state = 12345;
    g_sentinel_closes = 0;

    for (uint32_t i = 0; i < STRESS_PROCESSES; i++) {
        g_stress_processes[i] = test_helpers::create_unstarted_process_object(PROGRAM);
        ASSERT_NOT_NULL(g_stress_processes[i]);
        g_stress_process_handles[i] = handle_to(task, g_stress_processes[i]);
        ASSERT_TRUE(g_stress_process_handles[i] >= 0);
    }

    for (uint32_t i = 0; i < STRESS_SOCKETS; i += 2) {
        ASSERT_EQ(socket::create_socket_pair(&g_stress_sockets[i], &g_stress_sockets[i + 1]), resource::OK);
        g_stress_socket_handles[i] = handle_to(task, g_stress_sockets[i]);
        g_stress_socket_handles[i + 1] = handle_to(task, g_stress_sockets[i + 1]);
        ASSERT_TRUE(g_stress_socket_handles[i] >= 0 && g_stress_socket_handles[i + 1] >= 0);
    }

    ASSERT_TRUE(plant_sentinels());

    for (uint32_t round = 0; round < STRESS_ROUNDS; round++) {
        stress_round();
    }

    for (uint32_t i = 0; i < STRESS_PROCESSES; i++) {
        EXPECT_EQ(resource::close(task, g_stress_process_handles[i]), resource::OK);
        resource::resource_release(g_stress_processes[i]);
    }

    for (uint32_t i = 0; i < STRESS_SOCKETS; i++) {
        EXPECT_EQ(resource::close(task, g_stress_socket_handles[i]), resource::OK);
        resource::resource_release(g_stress_sockets[i]);
    }

    EXPECT_EQ(g_sentinel_closes, STRESS_PROCESSES + STRESS_SOCKETS);
}
