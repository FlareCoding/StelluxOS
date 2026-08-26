#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "common/ring_buffer.h"
#include "resource/resource.h"
#include "signals/signal.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(interrupt_results);

// The kill flag drives the same interruption logic as fatal signals
// and is safe to toggle on the running kernel test task.

static ring_buffer* g_rb;

static int32_t setup_rb() {
    RUN_ELEVATED({ g_rb = ring_buffer_create(64); });
    return g_rb ? 0 : -1;
}

static int32_t teardown_rb() {
    sched::current()->sig.pending.fetch_and_release(~signals::sig_bit(signals::SIGKILL));
    RUN_ELEVATED({
        if (g_rb) ring_buffer_destroy(g_rb);
    });
    g_rb = nullptr;
    return 0;
}

BEFORE_EACH(interrupt_results, setup_rb);
AFTER_EACH(interrupt_results, teardown_rb);

TEST(interrupt_results, interrupted_empty_read_is_not_eof) {
    uint8_t buf[8];
    ssize_t rc = 0;
    sched::current()->sig.pending.fetch_or_release(signals::sig_bit(signals::SIGKILL));
    RUN_ELEVATED({ rc = ring_buffer_read(g_rb, buf, sizeof(buf), false); });
    EXPECT_EQ(rc, RB_ERR_INTR);
}

TEST(interrupt_results, closed_writer_read_stays_eof_when_interrupted) {
    uint8_t buf[8];
    ssize_t rc = -100;
    RUN_ELEVATED({ ring_buffer_close_write(g_rb); });
    sched::current()->sig.pending.fetch_or_release(signals::sig_bit(signals::SIGKILL));
    RUN_ELEVATED({ rc = ring_buffer_read(g_rb, buf, sizeof(buf), false); });
    EXPECT_EQ(rc, 0LL);
}

// Fill regardless of how create rounds the requested capacity up
static void fill_buffer(ring_buffer* rb) {
    uint8_t byte = 0;
    ssize_t rc = 1;
    RUN_ELEVATED({
        while (rc == 1) {
            rc = ring_buffer_write(rb, &byte, 1, true);
        }
    });
}

TEST(interrupt_results, interrupted_full_write_is_not_epipe) {
    uint8_t buf[8];
    ssize_t rc = 0;
    fill_buffer(g_rb);
    sched::current()->sig.pending.fetch_or_release(signals::sig_bit(signals::SIGKILL));
    RUN_ELEVATED({ rc = ring_buffer_write(g_rb, buf, sizeof(buf), false); });
    EXPECT_EQ(rc, RB_ERR_INTR);

    rc = 0;
    RUN_ELEVATED({ rc = ring_buffer_write_all(g_rb, buf, sizeof(buf), false); });
    EXPECT_EQ(rc, RB_ERR_INTR);
}

TEST(interrupt_results, interrupted_write_with_space_still_writes) {
    uint8_t buf[8];
    ssize_t rc = 0;
    sched::current()->sig.pending.fetch_or_release(signals::sig_bit(signals::SIGKILL));
    RUN_ELEVATED({ rc = ring_buffer_write(g_rb, buf, sizeof(buf), false); });
    EXPECT_EQ(rc, 8LL);

    rc = 0;
    RUN_ELEVATED({ rc = ring_buffer_write_all(g_rb, buf, sizeof(buf), false); });
    EXPECT_EQ(rc, 8LL);
}

TEST(interrupt_results, closed_reader_write_stays_epipe_when_interrupted) {
    uint8_t buf[8];
    ssize_t rc = 0;
    RUN_ELEVATED({ ring_buffer_close_read(g_rb); });
    sched::current()->sig.pending.fetch_or_release(signals::sig_bit(signals::SIGKILL));
    RUN_ELEVATED({ rc = ring_buffer_write(g_rb, buf, sizeof(buf), false); });
    EXPECT_EQ(rc, RB_ERR_PIPE);
}

TEST(interrupt_results, rb_intr_matches_resource_code) {
    EXPECT_EQ(RB_ERR_INTR, static_cast<ssize_t>(resource::ERR_INTR));
    EXPECT_EQ(RB_ERR_PIPE, static_cast<ssize_t>(resource::ERR_PIPE));
}
