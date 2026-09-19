#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "harness.h"
#include "net/tcp/conn.h"
#include "net/tcp/listen.h"
#include "net/tcp/output.h"
#include "net/tcp/rtt.h"
#include "resource/resource.h"
#include "clock/clock.h"
#include "timer/timer.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(tcp_rto);

using namespace net;
using namespace net::tcp;

constexpr uint32_t PEER_ISS  = 9500;
constexpr uint16_t PEER_MSS  = 1000;
constexpr uint64_t MS        = 1000000ULL;

static uint64_t g_fake_now;
static uint8_t  g_bytes[2 * PEER_MSS];

static uint64_t fake_clock() {
    return g_fake_now;
}

static void advance_and_fire(uint64_t ns) {
    g_fake_now += ns;
    RUN_ELEVATED(timer::__dbg_test_fire_expired(g_fake_now));
}

static uint32_t sent_ts_val(const stub_interface& link, size_t frame) {
    tcp_options opts;
    parse_options(sent_tcp(link, frame), &opts);
    return opts.ts_val;
}

// A connection this host opened, its SYN-ACK arriving `handshake_rtt_ns`
// after the SYN, with timestamps when asked
struct measuring {
    rc::strong_ref<tcp_conn> conn;
    linked_peer&             lp;
    bool                     timestamps;
    uint32_t                 peer_ts;

    measuring(linked_peer& link, bool with_timestamps, uint64_t handshake_rtt_ns = 0)
        : lp(link), timestamps(with_timestamps), peer_ts(100) {
        g_fake_now = clock::now_ns();
        __dbg_test_set_clock(fake_clock);

        tuple key = {lp.remote.host, lp.remote.addr, lp.remote.host_port, lp.remote.port};
        open_active(key, &lp.link, &conn);
        uint32_t syn_ts = sent_ts_val(lp.link, 0);
        g_fake_now += handshake_rtt_ns;

        tcp_options opts;
        opts.mss = PEER_MSS;
        if (timestamps) {
            opts.has_timestamps = true;
            opts.ts_val = peer_ts;
            opts.ts_ecr = syn_ts;
        }

        input(lp.remote.segment(FLAG_SYN | FLAG_ACK, PEER_ISS, conn->iss + 1, opts));
        lp.link.clear_frames();
    }

    ~measuring() {
        abort_connection(conn.ptr());
        __dbg_test_set_clock(nullptr);
    }

    uint32_t first_seq() const { return conn->iss + 1; }

    void write(size_t len) {
        RUN_ELEVATED({
            sync::irq_lock_guard guard(conn->lock);
            (void)conn->snd_queue.append(g_bytes, len);
        });
        (void)output(conn.ptr());
    }

    // The peer acknowledging stream bytes below `first`, echoing `ts_ecr`
    int32_t ack(size_t first, uint32_t ts_ecr = 0) {
        tcp_options opts;
        if (timestamps) {
            opts.has_timestamps = true;
            opts.ts_val = ++peer_ts;
            opts.ts_ecr = ts_ecr;
        }

        return input(lp.remote.segment(FLAG_ACK, PEER_ISS + 1, first_seq() + static_cast<uint32_t>(first), opts));
    }
};

TEST(tcp_rto, the_first_sample_sets_the_estimate_and_later_ones_smooth_it) {
    linked_peer lp;
    measuring m(lp, false);
    EXPECT_EQ(m.conn->srtt_us, 0u);
    EXPECT_EQ(m.conn->rto_ns, TIMEOUT_INIT_NS);

    m.write(100);
    g_fake_now += 300 * MS;
    EXPECT_EQ(m.ack(100), OK);
    EXPECT_EQ(m.conn->srtt_us, 300000u);
    EXPECT_EQ(m.conn->rttvar_us, 150000u);
    EXPECT_EQ(m.conn->rto_ns, 900 * MS);

    m.write(100);
    g_fake_now += 500 * MS;
    EXPECT_EQ(m.ack(200), OK);
    EXPECT_EQ(m.conn->rttvar_us, 162500u);
    EXPECT_EQ(m.conn->srtt_us, 325000u);
    EXPECT_EQ(m.conn->rto_ns, 975 * MS);
}

TEST(tcp_rto, the_timeout_stays_between_its_floor_and_its_ceiling) {
    linked_peer lp;
    {
        measuring m(lp, false);
        m.write(100);
        g_fake_now += MS;
        EXPECT_EQ(m.ack(100), OK);
        EXPECT_EQ(m.conn->srtt_us, 1000u);
        EXPECT_EQ(m.conn->rto_ns, RTO_MIN_NS);
    }
    {
        measuring m(lp, false);
        m.write(100);
        g_fake_now += 100000 * MS;
        EXPECT_EQ(m.ack(100), OK);
        EXPECT_EQ(m.conn->rto_ns, TIMEOUT_MAX_NS);
    }
}

TEST(tcp_rto, a_segment_sent_twice_gives_no_sample_without_timestamps) {
    linked_peer lp;
    measuring m(lp, false);

    m.write(100);
    advance_and_fire(TIMEOUT_INIT_NS);
    EXPECT_EQ(m.conn->sent.oldest()->retrans, 1);

    g_fake_now += 100 * MS;
    EXPECT_EQ(m.ack(100), OK);
    EXPECT_EQ(m.conn->srtt_us, 0u);
    EXPECT_EQ(m.conn->rto_ns, TIMEOUT_INIT_NS);
}

TEST(tcp_rto, a_timestamp_echo_tells_which_transmission_was_acknowledged) {
    linked_peer lp;
    measuring m(lp, true);
    ASSERT_TRUE(m.conn->ts_ok);

    m.write(100);
    advance_and_fire(TIMEOUT_INIT_NS);
    ASSERT_EQ(lp.link.frames_sent(), 2u);
    uint32_t retransmission_ts = sent_ts_val(lp.link, 1);

    g_fake_now += 100 * MS;
    EXPECT_EQ(m.ack(100, retransmission_ts), OK);
    EXPECT_EQ(m.conn->srtt_us, 100000u);
    EXPECT_EQ(m.conn->rto_ns, 300 * MS);
}

TEST(tcp_rto, an_echo_of_a_timestamp_this_host_never_sent_is_ignored) {
    linked_peer lp;
    measuring m(lp, true);

    m.write(100);
    g_fake_now += 100 * MS;
    EXPECT_EQ(m.ack(100, sent_ts_val(lp.link, 0) + 1000000), OK);
    EXPECT_EQ(m.conn->srtt_us, 100000u);
}

TEST(tcp_rto, the_handshake_measures_the_first_round_trip) {
    linked_peer lp;
    measuring m(lp, true, 250 * MS);

    EXPECT_EQ(m.conn->srtt_us, 250000u);
    EXPECT_EQ(m.conn->rttvar_us, 125000u);
    EXPECT_EQ(m.conn->rto_ns, 750 * MS);
}

TEST(tcp_rto, the_passive_side_measures_the_handshake_too) {
    linked_peer lp;
    g_fake_now = clock::now_ns();
    __dbg_test_set_clock(fake_clock);
    tcp_listener* listener = alloc_listener(endpoint{ipv4::UNSPECIFIED_ADDR, 5000, nullptr, false});
    listener->backlog = 8;
    ASSERT_EQ(listener_insert(listener), OK);

    tcp_options syn_opts;
    syn_opts.mss = PEER_MSS;
    syn_opts.has_timestamps = true;
    syn_opts.ts_val = 500;
    EXPECT_EQ(input(lp.remote.segment(FLAG_SYN, PEER_ISS, 0, syn_opts)), OK);
    uint32_t synack_ts = sent_ts_val(lp.link, 0);
    uint32_t iss = ntohl(sent_tcp(lp.link, 0)->seq);

    g_fake_now += 200 * MS;
    tcp_options ack_opts;
    ack_opts.has_timestamps = true;
    ack_opts.ts_val = 501;
    ack_opts.ts_ecr = synack_ts;
    EXPECT_EQ(input(lp.remote.segment(FLAG_ACK, PEER_ISS + 1, iss + 1, ack_opts)), OK);

    rc::strong_ref<tcp_conn> conn = pop_accepted(listener);
    ASSERT_TRUE(conn);
    EXPECT_EQ(conn->srtt_us, 200000u);
    EXPECT_EQ(conn->rto_ns, 600 * MS);

    abort_connection(conn.ptr());
    listener_close(listener);
    if (listener->release()) {
        tcp_listener::ref_destroy(listener);
    }

    __dbg_test_set_clock(nullptr);
}

TEST(tcp_rto, a_retried_syn_leaves_no_backoff_behind_once_acknowledged) {
    linked_peer lp;
    g_fake_now = clock::now_ns();
    __dbg_test_set_clock(fake_clock);

    rc::strong_ref<tcp_conn> conn;
    tuple key = {lp.remote.host, lp.remote.addr, lp.remote.host_port, lp.remote.port};
    open_active(key, &lp.link, &conn);
    advance_and_fire(TIMEOUT_INIT_NS);
    advance_and_fire(2 * TIMEOUT_INIT_NS);
    ASSERT_EQ(conn->retransmits, 2);

    tcp_options opts;
    opts.mss = PEER_MSS;
    EXPECT_EQ(input(lp.remote.segment(FLAG_SYN | FLAG_ACK, PEER_ISS, conn->iss + 1, opts)), OK);
    ASSERT_EQ(conn->state, tcp_state::established);
    EXPECT_EQ(conn->retransmits, 0);

    RUN_ELEVATED({
        sync::irq_lock_guard guard(conn->lock);
        (void)conn->snd_queue.append(g_bytes, 100);
    });
    (void)output(conn.ptr());
    EXPECT_EQ(conn->send_timer_deadline_ns, g_fake_now + conn->rto_ns);

    abort_connection(conn.ptr());
    __dbg_test_set_clock(nullptr);
}

TEST(tcp_rto, the_timer_backs_off_from_the_estimate_and_starts_over_on_new_data) {
    linked_peer lp;
    measuring m(lp, false);

    m.write(100);
    g_fake_now += 300 * MS;
    EXPECT_EQ(m.ack(100), OK);
    ASSERT_EQ(m.conn->rto_ns, 900 * MS);

    m.write(100);
    EXPECT_EQ(m.conn->send_timer_deadline_ns, g_fake_now + 900 * MS);

    advance_and_fire(900 * MS);
    EXPECT_EQ(m.conn->retransmits, 1);
    EXPECT_EQ(m.conn->send_timer_deadline_ns, g_fake_now + 1800 * MS);

    advance_and_fire(1800 * MS);
    EXPECT_EQ(m.conn->retransmits, 2);
    EXPECT_EQ(m.conn->send_timer_deadline_ns, g_fake_now + 3600 * MS);

    EXPECT_EQ(m.ack(200), OK);
    EXPECT_EQ(m.conn->retransmits, 0);
    EXPECT_EQ(m.conn->rto_ns, 900 * MS);

    m.write(100);
    EXPECT_EQ(m.conn->send_timer_deadline_ns, g_fake_now + 900 * MS);
}
