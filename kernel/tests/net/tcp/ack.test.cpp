#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "harness.h"
#include "net/tcp/conn.h"
#include "resource/resource.h"
#include "clock/clock.h"
#include "timer/timer.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(tcp_ack);

using namespace net;
using namespace net::tcp;

constexpr uint32_t PEER_ISS = 6000;
constexpr uint16_t PEER_MSS = 1000;

static uint64_t g_fake_now;
static uint8_t  g_payload[2 * PEER_MSS];

static uint64_t fake_clock() {
    return g_fake_now;
}

static void advance_and_fire(uint64_t ns) {
    g_fake_now += ns;
    RUN_ELEVATED(timer::__dbg_test_fire_expired(g_fake_now));
}

// A connection this host opened, established with a peer whose MSS is
// PEER_MSS and that scales by nothing; rcv_nxt starts at PEER_ISS + 1
struct acknowledging {
    rc::strong_ref<tcp_conn> conn;
    linked_peer&             lp;
    size_t                   sent;

    explicit acknowledging(linked_peer& link) : lp(link), sent(0) {
        g_fake_now = clock::now_ns();
        __dbg_test_set_clock(fake_clock);

        tuple key = {lp.remote.host, lp.remote.addr, lp.remote.host_port, lp.remote.port};
        open_active(key, &lp.link, &conn);

        tcp_options opts;
        opts.mss = PEER_MSS;
        input(lp.remote.segment(FLAG_SYN | FLAG_ACK, PEER_ISS, conn->iss + 1, opts));
        lp.link.clear_frames();
    }

    ~acknowledging() {
        abort_connection(conn.ptr());
        __dbg_test_set_clock(nullptr);
    }

    uint32_t rcv_nxt() const { return PEER_ISS + 1 + static_cast<uint32_t>(sent); }

    // The next `len` bytes of the stream, in order, returning the frames they drew
    size_t send(size_t len, uint8_t flags = FLAG_ACK) {
        lp.link.clear_frames();
        input(lp.remote.segment(flags, rcv_nxt(), conn->iss + 1, {}, g_payload, len));
        sent += len;
        return lp.link.frames_sent();
    }

    void exhaust_quick_acks() {
        for (int i = 0; i < MAX_QUICKACKS; i++) {
            send(10);
        }

        lp.link.clear_frames();
    }
};

static void expect_ack(const acknowledging& a, size_t frame) {
    ASSERT_TRUE(a.lp.link.frames_sent() > frame);
    const tcp_header* hdr = sent_tcp(a.lp.link, frame);
    EXPECT_EQ(hdr->flags, FLAG_ACK);
    EXPECT_EQ(ntohl(hdr->ack), a.rcv_nxt());
}

TEST(tcp_ack, the_first_sixteen_segments_are_acknowledged_at_once_then_acks_wait) {
    linked_peer lp;
    acknowledging a(lp);
    EXPECT_EQ(a.conn->quick_acks, MAX_QUICKACKS);

    for (int i = 0; i < MAX_QUICKACKS; i++) {
        EXPECT_EQ(a.send(100), 1u);
        expect_ack(a, 0);
    }

    EXPECT_EQ(a.conn->quick_acks, 0);
    EXPECT_EQ(a.send(100), 0u);
    EXPECT_TRUE(a.conn->ack_pending);
    EXPECT_TRUE(a.conn->ack_timer_armed);
    EXPECT_EQ(a.conn->ack_timer_deadline_ns, g_fake_now + DELACK_NS);
    EXPECT_EQ(a.conn->rcv_acked, a.rcv_nxt() - 100);

    advance_and_fire(DELACK_NS);
    expect_ack(a, 0);
    EXPECT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_FALSE(a.conn->ack_pending);
    EXPECT_FALSE(a.conn->ack_timer_armed);
    EXPECT_EQ(a.conn->rcv_acked, a.rcv_nxt());
}

TEST(tcp_ack, two_full_segments_bring_the_ack_forward) {
    linked_peer lp;
    acknowledging a(lp);
    a.exhaust_quick_acks();

    EXPECT_EQ(a.send(PEER_MSS), 0u);
    EXPECT_TRUE(a.conn->ack_pending);

    EXPECT_EQ(a.send(PEER_MSS), 1u);
    expect_ack(a, 0);
    EXPECT_FALSE(a.conn->ack_pending);

    lp.link.clear_frames();
    advance_and_fire(DELACK_NS);
    EXPECT_EQ(lp.link.frames_sent(), 0u);
}

TEST(tcp_ack, payload_out_of_order_and_a_fin_are_acknowledged_at_once) {
    linked_peer lp;
    acknowledging a(lp);
    a.exhaust_quick_acks();

    lp.link.clear_frames();
    EXPECT_EQ(input(lp.remote.segment(FLAG_ACK, a.rcv_nxt() + 500, a.conn->iss + 1, {}, g_payload, 10)), OK);
    expect_ack(a, 0);
    EXPECT_FALSE(a.conn->ack_pending);

    EXPECT_EQ(a.send(10), 0u);
    EXPECT_TRUE(a.conn->ack_pending);

    lp.link.clear_frames();
    EXPECT_EQ(input(lp.remote.segment(FLAG_ACK | FLAG_FIN, a.rcv_nxt(), a.conn->iss + 1)), OK);
    a.sent++;
    expect_ack(a, 0);
    EXPECT_FALSE(a.conn->ack_pending);
    EXPECT_EQ(a.conn->state, tcp_state::close_wait);
}

TEST(tcp_ack, an_idle_period_restores_quick_acks) {
    linked_peer lp;
    acknowledging a(lp);
    a.exhaust_quick_acks();
    EXPECT_EQ(a.send(10), 0u);
    advance_and_fire(DELACK_NS);

    g_fake_now += TIMEOUT_INIT_NS + 1;
    EXPECT_EQ(a.send(10), 1u);
    expect_ack(a, 0);
    EXPECT_EQ(a.conn->quick_acks, MAX_QUICKACKS - 1);
}

TEST(tcp_ack, a_delayed_ack_goes_out_once_and_an_early_ack_cancels_it) {
    linked_peer lp;
    acknowledging a(lp);
    a.exhaust_quick_acks();

    EXPECT_EQ(a.send(10), 0u);
    advance_and_fire(DELACK_NS);
    expect_ack(a, 0);
    lp.link.clear_frames();
    advance_and_fire(DELACK_NS);
    EXPECT_EQ(lp.link.frames_sent(), 0u);

    EXPECT_EQ(a.send(10), 0u);
    EXPECT_TRUE(a.conn->ack_timer_armed);
    lp.link.clear_frames();
    EXPECT_EQ(input(lp.remote.segment(FLAG_ACK | FLAG_FIN, a.rcv_nxt(), a.conn->iss + 1)), OK);
    a.sent++;
    expect_ack(a, 0);
    EXPECT_FALSE(a.conn->ack_timer_armed);

    lp.link.clear_frames();
    advance_and_fire(DELACK_NS);
    EXPECT_EQ(lp.link.frames_sent(), 0u);
}

TEST(tcp_ack, closing_the_connection_carries_the_owed_ack) {
    linked_peer lp;
    acknowledging a(lp);
    a.exhaust_quick_acks();
    EXPECT_EQ(a.send(10), 0u);
    ASSERT_TRUE(a.conn->ack_pending);

    lp.link.clear_frames();
    close_connection(a.conn.ptr());
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(sent_tcp(lp.link, 0)->flags, FLAG_FIN | FLAG_ACK);
    EXPECT_EQ(ntohl(sent_tcp(lp.link, 0)->ack), a.rcv_nxt());
    EXPECT_FALSE(a.conn->ack_pending);

    lp.link.clear_frames();
    advance_and_fire(DELACK_NS);
    EXPECT_EQ(lp.link.frames_sent(), 0u);
}
