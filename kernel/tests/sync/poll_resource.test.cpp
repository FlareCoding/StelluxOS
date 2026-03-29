#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "dynpriv/dynpriv.h"
#include "sync/poll.h"
#include "sync/wait_queue.h"
#include "common/ring_buffer.h"
#include "resource/resource.h"
#include "pty/pty.h"
#include "socket/unix_socket.h"
#include "mm/heap.h"

TEST_SUITE(poll_resource);

// ---------------------------------------------------------------------------
// ring_buffer_poll_readable
// ---------------------------------------------------------------------------

TEST(poll_resource, ring_buffer_poll_readable) {
    ring_buffer* rb = nullptr;
    RUN_ELEVATED({ rb = ring_buffer_create(256); });
    ASSERT_NOT_NULL(rb);

    uint8_t data[] = {1, 2, 3};
    RUN_ELEVATED({ ring_buffer_write(rb, data, 3, true); });

    uint32_t mask = 0;
    RUN_ELEVATED({
        mask = ring_buffer_poll_read(rb, nullptr);
    });
    EXPECT_BITS_SET(mask, sync::POLL_IN);

    RUN_ELEVATED({ ring_buffer_destroy(rb); });
}

// ---------------------------------------------------------------------------
// ring_buffer_poll_not_readable
// ---------------------------------------------------------------------------

TEST(poll_resource, ring_buffer_poll_not_readable) {
    ring_buffer* rb = nullptr;
    RUN_ELEVATED({ rb = ring_buffer_create(256); });
    ASSERT_NOT_NULL(rb);

    uint32_t mask = 0;
    RUN_ELEVATED({
        mask = ring_buffer_poll_read(rb, nullptr);
    });
    EXPECT_EQ(mask, 0u);

    RUN_ELEVATED({ ring_buffer_destroy(rb); });
}

// ---------------------------------------------------------------------------
// ring_buffer_poll_writable
// ---------------------------------------------------------------------------

TEST(poll_resource, ring_buffer_poll_writable) {
    ring_buffer* rb = nullptr;
    RUN_ELEVATED({ rb = ring_buffer_create(256); });
    ASSERT_NOT_NULL(rb);

    uint32_t mask = 0;
    RUN_ELEVATED({
        mask = ring_buffer_poll_write(rb, nullptr);
    });
    EXPECT_BITS_SET(mask, sync::POLL_OUT);

    RUN_ELEVATED({ ring_buffer_destroy(rb); });
}

// ---------------------------------------------------------------------------
// ring_buffer_poll_hup
// ---------------------------------------------------------------------------

TEST(poll_resource, ring_buffer_poll_hup) {
    ring_buffer* rb = nullptr;
    RUN_ELEVATED({ rb = ring_buffer_create(256); });
    ASSERT_NOT_NULL(rb);

    RUN_ELEVATED({ ring_buffer_close_write(rb); });

    uint32_t mask = 0;
    RUN_ELEVATED({
        mask = ring_buffer_poll_read(rb, nullptr);
    });
    EXPECT_BITS_SET(mask, sync::POLL_HUP);

    RUN_ELEVATED({ ring_buffer_destroy(rb); });
}

// ---------------------------------------------------------------------------
// ring_buffer_poll_hup_with_data
// Writer closed but data remains: both POLL_IN and POLL_HUP must be set.
// ---------------------------------------------------------------------------

TEST(poll_resource, ring_buffer_poll_hup_with_data) {
    ring_buffer* rb = nullptr;
    RUN_ELEVATED({ rb = ring_buffer_create(256); });
    ASSERT_NOT_NULL(rb);

    uint8_t data[] = {1, 2, 3};
    RUN_ELEVATED({ ring_buffer_write(rb, data, 3, true); });
    RUN_ELEVATED({ ring_buffer_close_write(rb); });

    uint32_t mask = 0;
    RUN_ELEVATED({
        mask = ring_buffer_poll_read(rb, nullptr);
    });
    EXPECT_BITS_SET(mask, sync::POLL_IN);
    EXPECT_BITS_SET(mask, sync::POLL_HUP);

    RUN_ELEVATED({ ring_buffer_destroy(rb); });
}

// ---------------------------------------------------------------------------
// ring_buffer_poll_err
// ---------------------------------------------------------------------------

TEST(poll_resource, ring_buffer_poll_err) {
    ring_buffer* rb = nullptr;
    RUN_ELEVATED({ rb = ring_buffer_create(256); });
    ASSERT_NOT_NULL(rb);

    RUN_ELEVATED({ ring_buffer_close_read(rb); });

    uint32_t mask = 0;
    RUN_ELEVATED({
        mask = ring_buffer_poll_write(rb, nullptr);
    });
    EXPECT_BITS_SET(mask, sync::POLL_ERR);
    EXPECT_BITS_SET(mask, sync::POLL_OUT);

    RUN_ELEVATED({ ring_buffer_destroy(rb); });
}

// ---------------------------------------------------------------------------
// ring_buffer_poll_subscribes_read_wq
// ---------------------------------------------------------------------------

static sync::wait_queue g_rb_poll_wq;
static volatile uint32_t g_rb_poll_waiting;
static volatile uint32_t g_rb_poll_result;
static volatile uint32_t g_rb_poll_done;

static void rb_poll_waiter_fn(void* arg) {
    auto* rb = static_cast<ring_buffer*>(arg);
    RUN_ELEVATED({
        sync::poll_table pt;
        pt.init(sched::current());

        uint32_t mask = ring_buffer_poll_read(rb, &pt);
        if (mask & sync::POLL_IN) {
            __atomic_store_n(&g_rb_poll_result, 1, __ATOMIC_RELEASE);
            sync::poll_cleanup(pt);
            __atomic_store_n(&g_rb_poll_done, 1, __ATOMIC_RELEASE);
            sched::exit(0);
            return;
        }

        __atomic_store_n(&g_rb_poll_waiting, 1, __ATOMIC_RELEASE);
        sync::poll_wait(pt, 0);
        // Re-check after wake
        mask = ring_buffer_poll_read(rb, nullptr);
        __atomic_store_n(&g_rb_poll_result, (mask & sync::POLL_IN) ? 1 : 0, __ATOMIC_RELEASE);
        sync::poll_cleanup(pt);
    });
    __atomic_store_n(&g_rb_poll_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(poll_resource, ring_buffer_poll_subscribes_read_wq) {
    g_rb_poll_waiting = 0;
    g_rb_poll_result = 0;
    g_rb_poll_done = 0;

    ring_buffer* rb = nullptr;
    RUN_ELEVATED({ rb = ring_buffer_create(256); });
    ASSERT_NOT_NULL(rb);

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(rb_poll_waiter_fn, rb, "rb_poll");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(test_helpers::spin_wait(&g_rb_poll_waiting));
    test_helpers::brief_delay();

    // Write data to trigger the observer
    uint8_t data[] = {42};
    RUN_ELEVATED({ ring_buffer_write(rb, data, 1, true); });

    ASSERT_TRUE(test_helpers::spin_wait(&g_rb_poll_done));
    EXPECT_EQ(__atomic_load_n(&g_rb_poll_result, __ATOMIC_ACQUIRE), 1u);

    RUN_ELEVATED({ ring_buffer_destroy(rb); });
}

// ---------------------------------------------------------------------------
// pty_poll_readable
// ---------------------------------------------------------------------------

TEST(poll_resource, pty_poll_readable) {
    resource::resource_object* master = nullptr;
    resource::resource_object* slave = nullptr;

    RUN_ELEVATED({
        int32_t rc = pty::create_pair(&master, &slave);
        ASSERT_EQ(rc, 0);
    });
    ASSERT_NOT_NULL(master);
    ASSERT_NOT_NULL(slave);

    // Write to master → line discipline processes → slave can read.
    // Use newline to flush through cooked-mode line discipline.
    const char* msg = "x\n";
    RUN_ELEVATED({
        master->ops->write(master, msg, 2, 0);
    });

    // Poll slave for readability
    uint32_t mask = 0;
    RUN_ELEVATED({
        mask = slave->ops->poll(slave, nullptr);
    });
    EXPECT_BITS_SET(mask, sync::POLL_IN);

    RUN_ELEVATED({
        master->ops->close(master);
        slave->ops->close(slave);
        resource::resource_release(master);
        resource::resource_release(slave);
    });
}

// ---------------------------------------------------------------------------
// unix_socket_poll_readable
// ---------------------------------------------------------------------------

TEST(poll_resource, unix_socket_poll_readable) {
    resource::resource_object* a = nullptr;
    resource::resource_object* b = nullptr;

    RUN_ELEVATED({
        int32_t rc = socket::create_socket_pair(&a, &b);
        ASSERT_EQ(rc, 0);
    });
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);

    // Write to a → b can read
    const uint8_t data[] = {7};
    RUN_ELEVATED({
        a->ops->write(a, data, 1, 0);
    });

    uint32_t mask = 0;
    RUN_ELEVATED({
        mask = b->ops->poll(b, nullptr);
    });
    EXPECT_BITS_SET(mask, sync::POLL_IN);

    RUN_ELEVATED({
        a->ops->close(a);
        b->ops->close(b);
        resource::resource_release(a);
        resource::resource_release(b);
    });
}

// ---------------------------------------------------------------------------
// null_poll_table_just_probes
// ---------------------------------------------------------------------------

TEST(poll_resource, null_poll_table_just_probes) {
    ring_buffer* rb = nullptr;
    RUN_ELEVATED({ rb = ring_buffer_create(256); });
    ASSERT_NOT_NULL(rb);

    uint8_t data[] = {1};
    RUN_ELEVATED({ ring_buffer_write(rb, data, 1, true); });

    // Pass null pt — should just probe, no subscription
    uint32_t mask = 0;
    RUN_ELEVATED({
        mask = ring_buffer_poll_read(rb, nullptr);
    });
    EXPECT_BITS_SET(mask, sync::POLL_IN);

    // Verify no observers on the read_wq
    bool empty = false;
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(rb->read_wq.lock);
        empty = rb->read_wq.observers.empty();
        sync::spin_unlock_irqrestore(rb->read_wq.lock, irq);
    });
    EXPECT_TRUE(empty);

    RUN_ELEVATED({ ring_buffer_destroy(rb); });
}
