#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "harness.h"
#include "net/tcp/conn.h"
#include "net/tcp/congestion.h"
#include "net/tcp/recovery.h"
#include "net/tcp/output.h"
#include "net/net.h"
#include "clock/clock.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(tcp_recovery_dupack);

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

// A connection this host opened, its send queue full and 10 segments in flight
struct reordering {
    rc::strong_ref<tcp_conn> conn;
    linked_peer&             lp;

    explicit reordering(linked_peer& link) : lp(link) {
        g_fake_now = clock::now_ns();
        __dbg_test_set_clock(fake_clock);

        tuple key = {lp.remote.host, lp.remote.addr, lp.remote.host_port, lp.remote.port};
        open_active(key, &lp.link, &conn);

        tcp_options opts;
        opts.mss = PEER_MSS;
        input(segment_with_window(FLAG_SYN | FLAG_ACK, PEER_ISS, conn->iss + 1, opts, PEER_WINDOW));
        lp.link.clear_frames();

        write(6 * PEER_MSS);
        write(6 * PEER_MSS);
        write(6 * PEER_MSS);
        write(2 * PEER_MSS);
        lp.link.clear_frames();
    }

    ~reordering() {
        abort_connection(conn.ptr());
        __dbg_test_set_clock(nullptr);
    }

    uint32_t first_seq() const { return conn->iss + 1; }

    packet* segment_with_window(uint8_t flags, uint32_t seq, uint32_t ack, const tcp_options& opts, uint16_t window,
                                const void* payload = nullptr, size_t payload_len = 0) {
        packet* pkt = lp.remote.segment(flags, seq, ack, opts, payload, payload_len);
        tcp_header* hdr = reinterpret_cast<tcp_header*>(pkt->data());
        hdr->window = htons(window);
        hdr->checksum = 0;
        hdr->checksum = htons(compute_checksum(lp.remote.addr, lp.remote.host, hdr, pkt->length()));
        return pkt;
    }

    void write(size_t len) {
        RUN_ELEVATED({
            sync::irq_lock_guard guard(conn->lock);
            (void)conn->snd_queue.append(g_bytes, len);
        });
        (void)output(conn.ptr());
    }

    // The peer acknowledging stream bytes below `first` with `window`, carrying `payload_len` bytes
    int32_t ack(size_t first, uint16_t window = PEER_WINDOW, size_t payload_len = 0) {
        return input(segment_with_window(FLAG_ACK, PEER_ISS + 1, first_seq() + static_cast<uint32_t>(first), {},
                                         window, g_bytes, payload_len));
    }

    uint32_t in_flight() const { return conn->snd_nxt - conn->snd_una; }
};

static recovery_state g_states[4];
static size_t         g_state_count;

static void record_state(tcp_conn*, recovery_state state) {
    if (g_state_count < 4) {
        g_states[g_state_count] = state;
    }

    g_state_count++;
}

static uint32_t fake_ssthresh(tcp_conn*) { return 0; }
static void fake_grow(tcp_conn*, uint32_t) {}
static uint32_t fake_undo(tcp_conn* conn) { return conn->cwnd; }

static const congestion_ops RECORDING = {
    .name = "recording", .ssthresh = fake_ssthresh, .grow = fake_grow, .set_state = record_state, .undo = fake_undo};

static uint32_t sent_seq(const stub_interface& link, size_t frame) {
    return ntohl(sent_tcp(link, frame)->seq);
}

// Three duplicates: two new segments out, then the oldest one again
static void enter_recovery(reordering& r) {
    EXPECT_EQ(r.ack(0), OK);
    EXPECT_EQ(r.ack(0), OK);
    EXPECT_EQ(r.ack(0), OK);
    ASSERT_EQ(r.conn->recovery, recovery_state::recovery);
    ASSERT_EQ(r.in_flight(), 12u * PEER_MSS);
    r.lp.link.clear_frames();
}

TEST(tcp_recovery_dupack, the_first_two_duplicates_let_new_segments_out_and_the_third_retransmits_the_oldest) {
    linked_peer lp;
    reordering r(lp);
    ASSERT_EQ(r.in_flight(), 10u * PEER_MSS);
    ASSERT_EQ(r.conn->recovery, recovery_state::open);

    EXPECT_EQ(r.ack(0), OK);
    EXPECT_EQ(r.conn->dupacks, 1);
    EXPECT_EQ(r.conn->recovery, recovery_state::disorder);
    EXPECT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(sent_seq(lp.link, 0), r.first_seq() + 10 * PEER_MSS);
    EXPECT_EQ(r.conn->cwnd, 10u * PEER_MSS);

    EXPECT_EQ(r.ack(0), OK);
    EXPECT_EQ(r.conn->dupacks, 2);
    EXPECT_EQ(lp.link.frames_sent(), 2u);
    EXPECT_EQ(r.in_flight(), 12u * PEER_MSS);

    EXPECT_EQ(r.ack(0), OK);
    EXPECT_EQ(r.conn->dupacks, 3);
    EXPECT_EQ(r.conn->recovery, recovery_state::recovery);
    ASSERT_EQ(lp.link.frames_sent(), 3u);
    EXPECT_EQ(sent_seq(lp.link, 2), r.first_seq());
    EXPECT_EQ(r.in_flight(), 12u * PEER_MSS);
    EXPECT_EQ(r.conn->ssthresh, 5u * PEER_MSS);
    EXPECT_EQ(r.conn->cwnd, 8u * PEER_MSS);
    EXPECT_EQ(r.conn->prior_cwnd, 10u * PEER_MSS);
    EXPECT_EQ(r.conn->high_seq, r.first_seq() + 12 * PEER_MSS);
    EXPECT_EQ(r.conn->total_retransmits, 1u);
}

TEST(tcp_recovery_dupack, further_duplicates_inflate_the_window_until_new_data_fits) {
    linked_peer lp;
    reordering r(lp);
    enter_recovery(r);

    for (int i = 0; i < 4; i++) {
        EXPECT_EQ(r.ack(0), OK);
    }
    EXPECT_EQ(r.conn->cwnd, 12u * PEER_MSS);
    EXPECT_EQ(lp.link.frames_sent(), 0u);

    EXPECT_EQ(r.ack(0), OK);
    EXPECT_EQ(r.conn->cwnd, 13u * PEER_MSS);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(sent_seq(lp.link, 0), r.first_seq() + 12 * PEER_MSS);
    EXPECT_EQ(r.in_flight(), 13u * PEER_MSS);
}

TEST(tcp_recovery_dupack, a_partial_acknowledgment_retransmits_the_next_hole_and_deflates_the_window) {
    linked_peer lp;
    reordering r(lp);
    enter_recovery(r);

    EXPECT_EQ(r.ack(PEER_MSS), OK);
    EXPECT_EQ(r.conn->recovery, recovery_state::recovery);
    EXPECT_EQ(r.conn->dupacks, 0);
    EXPECT_EQ(r.conn->cwnd, 8u * PEER_MSS);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(sent_seq(lp.link, 0), r.first_seq() + PEER_MSS);

    EXPECT_EQ(r.ack(4 * PEER_MSS), OK);
    EXPECT_EQ(r.conn->cwnd, 6u * PEER_MSS);
    ASSERT_EQ(lp.link.frames_sent(), 2u);
    EXPECT_EQ(sent_seq(lp.link, 1), r.first_seq() + 4 * PEER_MSS);
    EXPECT_EQ(r.conn->total_retransmits, 3u);
}

TEST(tcp_recovery_dupack, a_full_acknowledgment_ends_recovery_with_the_window_settled_on_what_is_in_flight) {
    linked_peer lp;
    reordering r(lp);
    enter_recovery(r);

    EXPECT_EQ(r.ack(12 * PEER_MSS), OK);
    EXPECT_EQ(r.conn->recovery, recovery_state::open);
    EXPECT_EQ(r.conn->dupacks, 0);
    EXPECT_EQ(r.conn->cwnd, 2u * PEER_MSS);
    EXPECT_EQ(r.conn->ssthresh, 5u * PEER_MSS);
    EXPECT_TRUE(is_in_slow_start(r.conn.ptr()));
    EXPECT_EQ(lp.link.frames_sent(), 2u);
    EXPECT_EQ(r.in_flight(), 2u * PEER_MSS);
}

TEST(tcp_recovery_dupack, a_second_recovery_needs_acknowledgments_past_where_the_last_one_began) {
    linked_peer lp;
    reordering r(lp);
    enter_recovery(r);
    EXPECT_EQ(r.ack(12 * PEER_MSS), OK);
    ASSERT_EQ(r.in_flight(), 2u * PEER_MSS);
    lp.link.clear_frames();

    EXPECT_EQ(r.ack(12 * PEER_MSS), OK);
    EXPECT_EQ(r.ack(12 * PEER_MSS), OK);
    EXPECT_EQ(r.ack(12 * PEER_MSS), OK);
    EXPECT_EQ(r.conn->dupacks, 3);
    EXPECT_EQ(r.conn->recovery, recovery_state::disorder);
    EXPECT_EQ(r.conn->cwnd, 2u * PEER_MSS);

    EXPECT_EQ(r.ack(13 * PEER_MSS), OK);
    ASSERT_EQ(r.conn->recovery, recovery_state::open);
    lp.link.clear_frames();
    EXPECT_EQ(r.ack(13 * PEER_MSS), OK);
    EXPECT_EQ(r.ack(13 * PEER_MSS), OK);
    EXPECT_EQ(r.ack(13 * PEER_MSS), OK);
    EXPECT_EQ(r.conn->recovery, recovery_state::recovery);
    EXPECT_EQ(r.conn->high_seq, r.conn->snd_nxt);
    EXPECT_EQ(sent_seq(lp.link, lp.link.frames_sent() - 1), r.first_seq() + 13 * PEER_MSS);
}

TEST(tcp_recovery_dupack, an_acknowledgment_with_a_changed_window_or_payload_is_not_a_duplicate) {
    linked_peer lp;
    reordering r(lp);

    EXPECT_EQ(r.ack(0, PEER_WINDOW + 1000), OK);
    EXPECT_EQ(r.conn->dupacks, 0);
    EXPECT_EQ(r.conn->recovery, recovery_state::open);

    EXPECT_EQ(r.ack(0, PEER_WINDOW + 1000, 100), OK);
    EXPECT_EQ(r.conn->dupacks, 0);
    EXPECT_EQ(r.conn->recovery, recovery_state::open);
}

TEST(tcp_recovery_dupack, a_repeated_acknowledgment_with_nothing_outstanding_is_not_a_duplicate) {
    linked_peer lp;
    reordering r(lp);

    EXPECT_EQ(r.ack(10 * PEER_MSS), OK);
    size_t sent = r.conn->snd_nxt - r.first_seq();
    EXPECT_EQ(r.ack(sent), OK);
    ASSERT_EQ(r.in_flight(), 0u);

    EXPECT_EQ(r.ack(sent), OK);
    EXPECT_EQ(r.conn->dupacks, 0);
    EXPECT_EQ(r.conn->recovery, recovery_state::open);
}

TEST(tcp_recovery_dupack, limited_transmit_does_not_hide_that_the_window_was_the_limit) {
    linked_peer lp;
    reordering r(lp);
    RUN_ELEVATED({
        sync::irq_lock_guard guard(r.conn->lock);
        r.conn->ssthresh = r.conn->cwnd;
    });

    EXPECT_EQ(r.ack(0), OK);
    EXPECT_EQ(r.ack(0), OK);
    ASSERT_EQ(r.in_flight(), 12u * PEER_MSS);
    EXPECT_TRUE(is_cwnd_limited(r.conn.ptr()));

    EXPECT_EQ(r.ack(12 * PEER_MSS), OK);
    EXPECT_EQ(r.conn->recovery, recovery_state::open);
    EXPECT_EQ(r.conn->cwnd, 11u * PEER_MSS);
}

TEST(tcp_recovery_dupack, an_acknowledgment_that_moves_forward_ends_disorder) {
    linked_peer lp;
    reordering r(lp);

    EXPECT_EQ(r.ack(0), OK);
    EXPECT_EQ(r.ack(0), OK);
    ASSERT_EQ(r.conn->recovery, recovery_state::disorder);

    EXPECT_EQ(r.ack(3 * PEER_MSS), OK);
    EXPECT_EQ(r.conn->dupacks, 0);
    EXPECT_EQ(r.conn->recovery, recovery_state::open);
    EXPECT_EQ(limited_transmit_bytes(r.conn.ptr()), 0u);
    EXPECT_EQ(r.conn->cwnd, 12u * PEER_MSS);
}

TEST(tcp_recovery_dupack, the_algorithm_hears_each_change_of_state) {
    linked_peer lp;
    reordering r(lp);
    RUN_ELEVATED({
        sync::irq_lock_guard guard(r.conn->lock);
        r.conn->congestion = &RECORDING;
    });

    g_state_count = 0;
    EXPECT_EQ(r.ack(0), OK);
    EXPECT_EQ(r.ack(0), OK);
    EXPECT_EQ(r.ack(2 * PEER_MSS), OK);
    ASSERT_EQ(g_state_count, 2u);
    EXPECT_EQ(g_states[0], recovery_state::disorder);
    EXPECT_EQ(g_states[1], recovery_state::open);

    g_state_count = 0;
    EXPECT_EQ(r.ack(2 * PEER_MSS), OK);
    EXPECT_EQ(r.ack(2 * PEER_MSS), OK);
    EXPECT_EQ(r.ack(2 * PEER_MSS), OK);
    EXPECT_EQ(r.ack(r.conn->snd_nxt - r.first_seq()), OK);
    ASSERT_EQ(g_state_count, 3u);
    EXPECT_EQ(g_states[0], recovery_state::disorder);
    EXPECT_EQ(g_states[1], recovery_state::recovery);
    EXPECT_EQ(g_states[2], recovery_state::open);
}
