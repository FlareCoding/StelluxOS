#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "harness.h"
#include "net/tcp/conn.h"
#include "net/tcp/congestion.h"
#include "net/tcp/recovery.h"
#include "net/tcp/output.h"
#include "net/net.h"
#include "clock/clock.h"
#include "timer/timer.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(tcp_recovery_rto);

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

// A connection this host opened with `segments` written, 10 of them in flight
struct timing_out {
    rc::strong_ref<tcp_conn> conn;
    linked_peer&             lp;

    explicit timing_out(linked_peer& link, size_t segments = 20) : lp(link) {
        g_fake_now = clock::now_ns();
        __dbg_test_set_clock(fake_clock);

        tuple key = {lp.remote.host, lp.remote.addr, lp.remote.host_port, lp.remote.port};
        open_active(key, &lp.link, &conn);

        tcp_options opts;
        opts.mss = PEER_MSS;
        input(segment_with_window(FLAG_SYN | FLAG_ACK, PEER_ISS, conn->iss + 1, opts, PEER_WINDOW));
        lp.link.clear_frames();

        for (size_t left = segments; left > 0; left -= left < 6 ? left : 6) {
            write((left < 6 ? left : 6) * PEER_MSS);
        }
        lp.link.clear_frames();
    }

    ~timing_out() {
        abort_connection(conn.ptr());
        __dbg_test_set_clock(nullptr);
    }

    uint32_t first_seq() const { return conn->iss + 1; }
    uint32_t in_flight() const { return conn->snd_nxt - conn->snd_una; }

    packet* segment_with_window(uint8_t flags, uint32_t seq, uint32_t ack, const tcp_options& opts, uint16_t window) {
        packet* pkt = lp.remote.segment(flags, seq, ack, opts);
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

    int32_t ack(size_t first) {
        return input(segment_with_window(FLAG_ACK, PEER_ISS + 1, first_seq() + static_cast<uint32_t>(first), {},
                                         PEER_WINDOW));
    }

    void fire_rto() {
        g_fake_now += conn->rto_ns << conn->backoff;
        RUN_ELEVATED(timer::__dbg_test_fire_expired(g_fake_now));
    }
};

static uint32_t sent_seq(const stub_interface& link, size_t frame) {
    return ntohl(sent_tcp(link, frame)->seq);
}

TEST(tcp_recovery_rto, a_timeout_halves_the_threshold_and_leaves_one_segment_in_flight) {
    linked_peer lp;
    timing_out t(lp);
    ASSERT_EQ(t.in_flight(), 10u * PEER_MSS);

    t.fire_rto();
    EXPECT_EQ(t.conn->recovery, recovery_state::loss);
    EXPECT_EQ(t.conn->cwnd, PEER_MSS);
    EXPECT_EQ(t.conn->ssthresh, 5u * PEER_MSS);
    EXPECT_EQ(t.conn->prior_cwnd, 10u * PEER_MSS);
    EXPECT_EQ(t.conn->high_seq, t.conn->snd_nxt);
    EXPECT_EQ(t.conn->frto_step, FRTO_FIRST_ACK);
    EXPECT_EQ(t.conn->sent.lost_bytes(), 9u * PEER_MSS);
    EXPECT_EQ(pipe_bytes(t.conn.ptr()), PEER_MSS);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(sent_seq(lp.link, 0), t.first_seq());
    EXPECT_EQ(t.conn->total_retransmits, 1u);
}

TEST(tcp_recovery_rto, a_repeated_timeout_keeps_the_threshold_and_gives_up_on_frto) {
    linked_peer lp;
    timing_out t(lp);

    t.fire_rto();
    t.fire_rto();
    EXPECT_EQ(t.conn->recovery, recovery_state::loss);
    EXPECT_EQ(t.conn->cwnd, PEER_MSS);
    EXPECT_EQ(t.conn->ssthresh, 5u * PEER_MSS);
    EXPECT_EQ(t.conn->frto_step, 0);
    EXPECT_EQ(t.conn->sent.lost_bytes(), 9u * PEER_MSS);
    EXPECT_EQ(lp.link.frames_sent(), 2u);
    EXPECT_EQ(sent_seq(lp.link, 1), t.first_seq());
}

TEST(tcp_recovery_rto, after_a_genuine_timeout_the_lost_segments_go_again_ahead_of_new_data) {
    linked_peer lp;
    timing_out t(lp);
    t.fire_rto();
    lp.link.clear_frames();

    EXPECT_EQ(t.ack(0), OK);
    EXPECT_EQ(t.conn->frto_step, 0);
    EXPECT_EQ(lp.link.frames_sent(), 0u);

    EXPECT_EQ(t.ack(PEER_MSS), OK);
    EXPECT_EQ(t.conn->recovery, recovery_state::loss);
    EXPECT_EQ(t.conn->cwnd, 2u * PEER_MSS);
    ASSERT_EQ(lp.link.frames_sent(), 2u);
    EXPECT_EQ(sent_seq(lp.link, 0), t.first_seq() + PEER_MSS);
    EXPECT_EQ(sent_seq(lp.link, 1), t.first_seq() + 2 * PEER_MSS);
    EXPECT_EQ(t.conn->sent.lost_bytes(), 7u * PEER_MSS);
    EXPECT_EQ(pipe_bytes(t.conn.ptr()), 2u * PEER_MSS);

    EXPECT_EQ(t.ack(3 * PEER_MSS), OK);
    EXPECT_EQ(t.conn->cwnd, 4u * PEER_MSS);
    ASSERT_EQ(lp.link.frames_sent(), 6u);
    EXPECT_EQ(sent_seq(lp.link, 2), t.first_seq() + 3 * PEER_MSS);
    EXPECT_EQ(sent_seq(lp.link, 5), t.first_seq() + 6 * PEER_MSS);
    EXPECT_EQ(t.conn->sent.lost_bytes(), 3u * PEER_MSS);
}

TEST(tcp_recovery_rto, lost_segments_go_again_even_when_nothing_new_is_left_to_send) {
    linked_peer lp;
    timing_out t(lp, 10);
    ASSERT_EQ(unsent_bytes(t.conn.ptr()), 0u);
    t.fire_rto();
    EXPECT_EQ(t.conn->frto_step, FRTO_FIRST_ACK);
    lp.link.clear_frames();

    EXPECT_EQ(t.ack(PEER_MSS), OK);
    EXPECT_EQ(t.conn->frto_step, 0);
    EXPECT_EQ(t.conn->cwnd, 2u * PEER_MSS);
    ASSERT_EQ(lp.link.frames_sent(), 2u);
    EXPECT_EQ(sent_seq(lp.link, 0), t.first_seq() + PEER_MSS);
    EXPECT_EQ(sent_seq(lp.link, 1), t.first_seq() + 2 * PEER_MSS);
    EXPECT_EQ(t.conn->sent.lost_bytes(), 7u * PEER_MSS);
}

TEST(tcp_recovery_rto, loss_recovery_ends_once_everything_sent_at_the_timeout_is_acknowledged) {
    linked_peer lp;
    timing_out t(lp);
    t.fire_rto();
    EXPECT_EQ(t.ack(0), OK);

    EXPECT_EQ(t.ack(10 * PEER_MSS), OK);
    EXPECT_EQ(t.conn->recovery, recovery_state::open);
    EXPECT_EQ(t.conn->frto_step, 0);
    EXPECT_EQ(t.conn->dupacks, 0);
    EXPECT_EQ(t.conn->sent.lost_bytes(), 0u);
    EXPECT_EQ(t.conn->cwnd, PEER_MSS);
    EXPECT_TRUE(is_in_slow_start(t.conn.ptr()));
}

TEST(tcp_recovery_rto, frto_sends_new_data_on_the_first_acknowledgment_and_undoes_a_spurious_timeout) {
    linked_peer lp;
    timing_out t(lp);
    t.fire_rto();
    lp.link.clear_frames();

    EXPECT_EQ(t.ack(PEER_MSS), OK);
    EXPECT_EQ(t.conn->frto_step, FRTO_SECOND_ACK);
    EXPECT_EQ(t.conn->cwnd, 2u * PEER_MSS);
    ASSERT_EQ(lp.link.frames_sent(), 2u);
    EXPECT_EQ(sent_seq(lp.link, 0), t.first_seq() + 10 * PEER_MSS);
    EXPECT_EQ(sent_seq(lp.link, 1), t.first_seq() + 11 * PEER_MSS);
    EXPECT_EQ(t.conn->sent.lost_bytes(), 9u * PEER_MSS);

    EXPECT_EQ(t.ack(5 * PEER_MSS), OK);
    EXPECT_EQ(t.conn->recovery, recovery_state::open);
    EXPECT_EQ(t.conn->frto_step, 0);
    EXPECT_EQ(t.conn->cwnd, 10u * PEER_MSS);
    EXPECT_EQ(t.conn->ssthresh, SSTHRESH_INFINITE);
    EXPECT_EQ(t.conn->sent.lost_bytes(), 0u);
    EXPECT_EQ(pipe_bytes(t.conn.ptr()), t.in_flight());
}

TEST(tcp_recovery_rto, frto_falls_back_to_retransmitting_when_the_second_acknowledgment_is_a_duplicate) {
    linked_peer lp;
    timing_out t(lp);
    t.fire_rto();
    EXPECT_EQ(t.ack(PEER_MSS), OK);
    ASSERT_EQ(t.conn->frto_step, FRTO_SECOND_ACK);
    lp.link.clear_frames();

    EXPECT_EQ(t.ack(PEER_MSS), OK);
    EXPECT_EQ(t.conn->frto_step, 0);
    EXPECT_EQ(t.conn->recovery, recovery_state::loss);
    EXPECT_EQ(t.conn->cwnd, 3u * PEER_MSS);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(sent_seq(lp.link, 0), t.first_seq() + PEER_MSS);
    EXPECT_EQ(t.conn->sent.lost_bytes(), 8u * PEER_MSS);
}

TEST(tcp_recovery_rto, duplicates_during_loss_recovery_start_no_fast_retransmit) {
    linked_peer lp;
    timing_out t(lp);
    t.fire_rto();
    lp.link.clear_frames();

    for (int i = 0; i < 4; i++) {
        EXPECT_EQ(t.ack(0), OK);
    }
    EXPECT_EQ(t.conn->recovery, recovery_state::loss);
    EXPECT_EQ(t.conn->dupacks, 4);
    EXPECT_EQ(t.conn->cwnd, PEER_MSS);
    EXPECT_EQ(lp.link.frames_sent(), 0u);
}

TEST(tcp_recovery_rto, a_timeout_during_fast_recovery_ends_it_and_moves_the_recovery_point) {
    linked_peer lp;
    timing_out t(lp);
    EXPECT_EQ(t.ack(0), OK);
    EXPECT_EQ(t.ack(0), OK);
    EXPECT_EQ(t.ack(0), OK);
    ASSERT_EQ(t.conn->recovery, recovery_state::recovery);
    ASSERT_EQ(t.conn->cwnd, 8u * PEER_MSS);
    uint32_t recovery_point = t.conn->high_seq;

    t.fire_rto();
    EXPECT_EQ(t.conn->recovery, recovery_state::loss);
    EXPECT_EQ(t.conn->cwnd, PEER_MSS);
    EXPECT_EQ(t.conn->high_seq, t.conn->snd_nxt);
    EXPECT_EQ(t.conn->high_seq, recovery_point);
    EXPECT_EQ(t.conn->dupacks, 0);

    EXPECT_EQ(t.ack(0), OK);
    EXPECT_EQ(t.ack(0), OK);
    EXPECT_EQ(t.ack(0), OK);
    EXPECT_EQ(t.conn->recovery, recovery_state::loss);
    EXPECT_EQ(t.conn->cwnd, PEER_MSS);
}
