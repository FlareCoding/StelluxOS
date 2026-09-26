#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "harness.h"
#include "net/tcp/conn.h"
#include "net/tcp/congestion.h"
#include "net/tcp/output.h"
#include "net/net.h"
#include "clock/clock.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(tcp_newreno);

using namespace net;
using namespace net::tcp;

constexpr uint32_t PEER_ISS    = 8000;
constexpr uint16_t PEER_MSS    = 1000;
constexpr uint16_t PEER_WINDOW = 60000;

static uint64_t g_fake_now;
static uint8_t  g_bytes[6 * PEER_MSS];

static uint64_t fake_clock() {
    return g_fake_now;
}

// A connection this host opened to a peer offering PEER_MSS and PEER_WINDOW
struct growing {
    rc::strong_ref<tcp_conn> conn;
    linked_peer&             lp;

    explicit growing(linked_peer& link) : lp(link) {
        g_fake_now = clock::now_ns();
        __dbg_test_set_clock(fake_clock);

        tuple key = {lp.remote.host, lp.remote.addr, lp.remote.host_port, lp.remote.port};
        open_active(key, &lp.link, &conn);

        tcp_options opts;
        opts.mss = PEER_MSS;
        packet* synack = lp.remote.segment(FLAG_SYN | FLAG_ACK, PEER_ISS, conn->iss + 1, opts);
        tcp_header* hdr = reinterpret_cast<tcp_header*>(synack->data());
        hdr->window = htons(PEER_WINDOW);
        hdr->checksum = 0;
        hdr->checksum = htons(compute_checksum(lp.remote.addr, lp.remote.host, hdr, synack->length()));
        input(synack);
        lp.link.clear_frames();
    }

    ~growing() {
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

    int32_t ack(size_t first) {
        packet* pkt = lp.remote.segment(FLAG_ACK, PEER_ISS + 1, first_seq() + static_cast<uint32_t>(first));
        tcp_header* hdr = reinterpret_cast<tcp_header*>(pkt->data());
        hdr->window = htons(PEER_WINDOW);
        hdr->checksum = 0;
        hdr->checksum = htons(compute_checksum(lp.remote.addr, lp.remote.host, hdr, pkt->length()));
        return input(pkt);
    }

    template <typename F>
    void adjust(F&& change) {
        RUN_ELEVATED({
            sync::irq_lock_guard guard(conn->lock);
            change(conn.ptr());
        });
    }
};

static uint32_t fake_ssthresh(tcp_conn*) { return 0; }
static void fake_grow(tcp_conn*, uint32_t) {}
static uint32_t fake_undo(tcp_conn* conn) { return conn->cwnd; }

static congestion_event g_events[4];
static size_t           g_event_count;

static void record_event(tcp_conn*, congestion_event which) {
    if (g_event_count < 4) {
        g_events[g_event_count] = which;
    }

    g_event_count++;
}

static const congestion_ops RECORDING = {
    .name = "recording", .ssthresh = fake_ssthresh, .grow = fake_grow, .event = record_event, .undo = fake_undo};

// Grows the window to 16 segments and acknowledges everything, leaving nothing in flight
static void grow_and_drain(growing& g) {
    g.write(6 * PEER_MSS);
    g.write(6 * PEER_MSS);
    EXPECT_EQ(g.ack(2 * PEER_MSS), OK);
    EXPECT_EQ(g.ack(4 * PEER_MSS), OK);
    EXPECT_EQ(g.ack(12 * PEER_MSS), OK);
    ASSERT_EQ(g.conn->cwnd, 16u * PEER_MSS);
    ASSERT_EQ(g.conn->snd_nxt, g.conn->snd_una);
}

TEST(tcp_newreno, the_initial_window_follows_rfc_6928) {
    EXPECT_EQ(initial_window(1460), 14600u);
    EXPECT_EQ(initial_window(536), 5360u);
    EXPECT_EQ(initial_window(100), 1000u);
    EXPECT_EQ(initial_window(8000), 16000u);
}

TEST(tcp_newreno, the_registry_names_algorithms_and_refuses_a_taken_name) {
    const congestion_ops* newreno = find_congestion_ops("newreno");
    ASSERT_NOT_NULL(newreno);
    EXPECT_EQ(default_congestion_ops(), newreno);
    EXPECT_TRUE(find_congestion_ops("nothing") == nullptr);

    congestion_ops twin = {.name = "newreno", .ssthresh = fake_ssthresh, .grow = fake_grow, .undo = fake_undo};
    EXPECT_EQ(register_congestion_ops(&twin), ERR_IN_USE);
    EXPECT_EQ(find_congestion_ops("newreno"), newreno);
}

TEST(tcp_newreno, a_new_connection_starts_in_slow_start_with_the_default_algorithm) {
    linked_peer lp;
    growing g(lp);

    EXPECT_EQ(g.conn->congestion, default_congestion_ops());
    EXPECT_EQ(g.conn->cwnd, 10u * PEER_MSS);
    EXPECT_EQ(g.conn->ssthresh, SSTHRESH_INFINITE);
    EXPECT_EQ(g.conn->recovery, recovery_state::open);
    EXPECT_TRUE(is_in_slow_start(g.conn.ptr()));
    EXPECT_FALSE(is_cwnd_limited(g.conn.ptr()));
}

TEST(tcp_newreno, slow_start_grows_by_what_was_acknowledged_while_the_window_is_the_limit) {
    linked_peer lp;
    growing g(lp);

    g.write(6 * PEER_MSS);
    g.write(6 * PEER_MSS);
    ASSERT_EQ(g.conn->snd_nxt - g.first_seq(), 10u * PEER_MSS);
    EXPECT_TRUE(is_cwnd_limited(g.conn.ptr()));

    EXPECT_EQ(g.ack(2 * PEER_MSS), OK);
    EXPECT_EQ(g.conn->cwnd, 12u * PEER_MSS);
    EXPECT_EQ(g.ack(4 * PEER_MSS), OK);
    EXPECT_EQ(g.conn->cwnd, 14u * PEER_MSS);
    EXPECT_EQ(g.conn->snd_nxt - g.first_seq(), 12u * PEER_MSS);
}

TEST(tcp_newreno, an_acknowledgment_of_more_than_two_segments_grows_the_window_by_two) {
    linked_peer lp;
    growing g(lp);

    g.write(6 * PEER_MSS);
    g.write(6 * PEER_MSS);
    ASSERT_TRUE(is_cwnd_limited(g.conn.ptr()));

    EXPECT_EQ(g.ack(5 * PEER_MSS), OK);
    EXPECT_EQ(g.conn->cwnd, 12u * PEER_MSS);
}

TEST(tcp_newreno, the_window_does_not_grow_for_a_sender_the_application_limits) {
    linked_peer lp;
    growing g(lp);

    g.write(PEER_MSS);
    EXPECT_FALSE(is_cwnd_limited(g.conn.ptr()));
    EXPECT_EQ(g.ack(PEER_MSS), OK);
    EXPECT_EQ(g.conn->cwnd, 10u * PEER_MSS);
}

TEST(tcp_newreno, slow_start_stops_at_the_threshold_and_hands_the_rest_to_congestion_avoidance) {
    linked_peer lp;
    growing g(lp);
    g.adjust([](tcp_conn* conn) {
        conn->cwnd = 9 * PEER_MSS;
        conn->ssthresh = 10 * PEER_MSS;
    });

    uint32_t left = 0;
    g.adjust([&](tcp_conn* conn) { left = slow_start(conn, 2 * PEER_MSS); });
    EXPECT_EQ(g.conn->cwnd, 10u * PEER_MSS);
    EXPECT_EQ(left, PEER_MSS);
    EXPECT_FALSE(is_in_slow_start(g.conn.ptr()));

    g.adjust([&](tcp_conn* conn) { additive_increase(conn, conn->cwnd, left); });
    EXPECT_EQ(g.conn->cwnd, 10u * PEER_MSS);
    EXPECT_EQ(g.conn->bytes_acked, PEER_MSS);
}

TEST(tcp_newreno, congestion_avoidance_adds_one_segment_per_window_acknowledged) {
    linked_peer lp;
    growing g(lp);
    g.adjust([](tcp_conn* conn) { conn->ssthresh = conn->cwnd; });

    for (int i = 0; i < 5; i++) {
        g.write(6 * PEER_MSS);
    }
    ASSERT_TRUE(is_cwnd_limited(g.conn.ptr()));
    EXPECT_FALSE(is_in_slow_start(g.conn.ptr()));

    size_t acked = 0;
    for (int i = 0; i < 4; i++) {
        acked += 2 * PEER_MSS;
        EXPECT_EQ(g.ack(acked), OK);
        EXPECT_EQ(g.conn->cwnd, 10u * PEER_MSS);
    }

    acked += 2 * PEER_MSS;
    EXPECT_EQ(g.ack(acked), OK);
    EXPECT_EQ(g.conn->cwnd, 11u * PEER_MSS);
    EXPECT_EQ(g.conn->bytes_acked, 0u);
}

TEST(tcp_newreno, a_send_after_an_idle_longer_than_the_timeout_restarts_from_the_initial_window) {
    linked_peer lp;
    growing g(lp);
    grow_and_drain(g);

    g_fake_now += g.conn->rto_ns + 1;
    g.write(PEER_MSS);
    EXPECT_EQ(g.conn->cwnd, 10u * PEER_MSS);
    EXPECT_EQ(g.conn->ssthresh, SSTHRESH_INFINITE);
    EXPECT_EQ(g.conn->snd_nxt - g.first_seq(), 13u * PEER_MSS);
}

TEST(tcp_newreno, a_pause_within_the_timeout_keeps_the_window) {
    linked_peer lp;
    growing g(lp);
    grow_and_drain(g);

    g_fake_now += g.conn->rto_ns / 2;
    g.write(PEER_MSS);
    EXPECT_EQ(g.conn->cwnd, 16u * PEER_MSS);
}

TEST(tcp_newreno, a_restart_keeps_three_quarters_of_the_window_as_the_threshold) {
    linked_peer lp;
    growing g(lp);
    grow_and_drain(g);
    g.adjust([](tcp_conn* conn) { conn->ssthresh = 5 * PEER_MSS; });

    g_fake_now += g.conn->rto_ns + 1;
    g.write(PEER_MSS);
    EXPECT_EQ(g.conn->cwnd, 10u * PEER_MSS);
    EXPECT_EQ(g.conn->ssthresh, 12u * PEER_MSS);
    EXPECT_EQ(g.conn->bytes_acked, 0u);
}

TEST(tcp_newreno, the_algorithm_hears_the_restart_and_then_the_start_of_transmission) {
    linked_peer lp;
    growing g(lp);
    grow_and_drain(g);
    g.adjust([](tcp_conn* conn) { conn->congestion = &RECORDING; });

    g_event_count = 0;
    g_fake_now += g.conn->rto_ns / 2;
    g.write(PEER_MSS);
    ASSERT_EQ(g_event_count, 1u);
    EXPECT_EQ(g_events[0], congestion_event::tx_start);

    EXPECT_EQ(g.ack(13 * PEER_MSS), OK);
    g_event_count = 0;
    g_fake_now += g.conn->rto_ns + 1;
    g.write(PEER_MSS);
    ASSERT_EQ(g_event_count, 2u);
    EXPECT_EQ(g_events[0], congestion_event::cwnd_restart);
    EXPECT_EQ(g_events[1], congestion_event::tx_start);
}

TEST(tcp_newreno, the_threshold_is_half_the_window_but_at_least_two_segments) {
    linked_peer lp;
    growing g(lp);
    const congestion_ops* newreno = g.conn->congestion;

    EXPECT_EQ(newreno->ssthresh(g.conn.ptr()), 5u * PEER_MSS);
    g.adjust([](tcp_conn* conn) { conn->cwnd = 3 * PEER_MSS; });
    EXPECT_EQ(newreno->ssthresh(g.conn.ptr()), 2u * PEER_MSS);
}

TEST(tcp_newreno, undo_restores_the_larger_of_the_window_and_the_one_before_the_loss) {
    linked_peer lp;
    growing g(lp);
    const congestion_ops* newreno = g.conn->congestion;

    g.adjust([](tcp_conn* conn) {
        conn->cwnd = 5 * PEER_MSS;
        conn->prior_cwnd = 8 * PEER_MSS;
    });
    EXPECT_EQ(newreno->undo(g.conn.ptr()), 8u * PEER_MSS);

    g.adjust([](tcp_conn* conn) { conn->prior_cwnd = 4 * PEER_MSS; });
    EXPECT_EQ(newreno->undo(g.conn.ptr()), 5u * PEER_MSS);
}
