#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "harness.h"
#include "net/tcp/conn.h"
#include "net/tcp/output.h"
#include "net/tcp/recovery.h"
#include "net/icmp.h"
#include "net/net.h"
#include "resource/resource.h"
#include "clock/clock.h"
#include "timer/timer.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(tcp_pmtu);

using namespace net;
using namespace net::tcp;

constexpr uint32_t PEER_ISS  = 8000;
constexpr uint16_t PEER_MSS  = 1460;
constexpr uint16_t LINK_MTU  = 1500;
constexpr uint16_t HEADERS   = ipv4::HEADER_LEN + HEADER_LEN;

static uint8_t  g_bytes[10 * PEER_MSS];
static uint64_t g_fake_now;

static uint64_t fake_clock() {
    return g_fake_now;
}

// An established connection whose path a router may report narrower than the link
struct narrowed {
    rc::strong_ref<tcp_conn> conn;
    linked_peer&             lp;

    explicit narrowed(linked_peer& link) : lp(link) {
        g_fake_now = clock::now_ns();
        __dbg_test_set_clock(fake_clock);

        open_active(tuple{lp.remote.host, lp.remote.addr, lp.remote.host_port, lp.remote.port}, &lp.link, &conn);
        tcp_options opts;
        opts.mss = PEER_MSS;
        input(lp.remote.segment(FLAG_SYN | FLAG_ACK, PEER_ISS, conn->iss + 1, opts));
        lp.link.clear_frames();
    }

    ~narrowed() {
        abort_connection(conn.ptr());
        __dbg_test_set_clock(nullptr);
    }

    void fire_rto() {
        g_fake_now += conn->rto_ns << conn->backoff;
        RUN_ELEVATED(timer::__dbg_test_fire_expired(g_fake_now));
    }

    void write(size_t len) {
        RUN_ELEVATED({
            sync::irq_lock_guard guard(conn->lock);
            (void)conn->snd_queue.append(g_bytes, len);
        });
        (void)output(conn.ptr());
    }

    int32_t fragmentation_needed(uint16_t next_hop_mtu, uint32_t offending_seq) {
        return icmp::input(lp.remote.icmp_error(icmp::TYPE_DEST_UNREACHABLE, icmp::CODE_FRAGMENTATION_NEEDED,
                                                offending_seq, next_hop_mtu));
    }

    int32_t ack(uint32_t ack_seq) {
        return input(lp.remote.segment(FLAG_ACK, PEER_ISS + 1, ack_seq));
    }
};

static size_t sent_payload_len(const stub_interface& link, size_t frame) {
    const tcp_header* hdr = sent_tcp(link, frame);
    return link.frame_len(frame) - eth::HEADER_LEN - ipv4::HEADER_LEN - hdr->header_len();
}

static uint32_t sent_seq(const stub_interface& link, size_t frame) {
    return ntohl(sent_tcp(link, frame)->seq);
}

TEST(tcp_pmtu, the_announced_mtu_becomes_the_path_and_bounds_the_mss) {
    linked_peer lp;
    narrowed n(lp);
    n.write(PEER_MSS);
    ASSERT_EQ(n.conn->snd_mss, PEER_MSS);

    EXPECT_EQ(n.fragmentation_needed(1000, n.conn->snd_una), OK);
    EXPECT_EQ(n.conn->path_mtu, 1000);
    EXPECT_EQ(n.conn->snd_mss, 1000 - HEADERS);
    EXPECT_EQ(n.conn->soft_error, OK);
}

TEST(tcp_pmtu, records_too_long_for_the_path_go_again_in_pieces_that_fit) {
    linked_peer lp;
    narrowed n(lp);
    n.write(2 * PEER_MSS);
    ASSERT_EQ(lp.link.frames_sent(), 2u);
    lp.link.clear_frames();
    uint32_t una = n.conn->snd_una;
    uint32_t mss = 1000 - HEADERS;

    EXPECT_EQ(n.fragmentation_needed(1000, una), OK);
    ASSERT_EQ(lp.link.frames_sent(), 4u);
    EXPECT_EQ(sent_seq(lp.link, 0), una);
    EXPECT_EQ(sent_payload_len(lp.link, 0), mss);
    EXPECT_EQ(sent_seq(lp.link, 1), una + mss);
    EXPECT_EQ(sent_payload_len(lp.link, 1), PEER_MSS - mss);
    EXPECT_EQ(sent_seq(lp.link, 2), una + PEER_MSS);
    EXPECT_EQ(sent_payload_len(lp.link, 2), mss);
    EXPECT_EQ(sent_seq(lp.link, 3), una + PEER_MSS + mss);
    EXPECT_EQ(sent_payload_len(lp.link, 3), PEER_MSS - mss);
    EXPECT_EQ(n.conn->sent.count(), 4u);
    EXPECT_EQ(n.conn->sent.lost_bytes(), 0u);
    EXPECT_EQ(n.conn->total_retransmits, 4u);
}

TEST(tcp_pmtu, the_record_cap_follows_the_mss) {
    linked_peer lp;
    narrowed n(lp);
    n.write(PEER_MSS);
    size_t queue_bytes = n.conn->snd_queue.limit() * CHUNK_PAYLOAD;
    ASSERT_EQ(n.conn->sent.cap(), queue_bytes / PEER_MSS + SENT_SEGMENT_MARGIN);

    EXPECT_EQ(n.fragmentation_needed(1000, n.conn->snd_una), OK);
    EXPECT_EQ(n.conn->sent.cap(), queue_bytes / (1000 - HEADERS) + SENT_SEGMENT_MARGIN);
}

TEST(tcp_pmtu, a_full_window_splits_past_the_record_cap) {
    linked_peer lp;
    narrowed n(lp);
    n.write(10 * PEER_MSS);
    ASSERT_EQ(lp.link.frames_sent(), 10u);
    ASSERT_EQ(n.conn->sent.cap(), 19u);
    lp.link.clear_frames();
    uint32_t mss = 1492 - HEADERS;

    EXPECT_EQ(n.fragmentation_needed(1492, n.conn->snd_una), OK);
    EXPECT_EQ(lp.link.frames_sent(), MAX_BURST);
    EXPECT_EQ(n.conn->sent.count(), 18u);
    EXPECT_EQ(n.conn->sent.lost_bytes(), 2u * PEER_MSS);
    for (size_t frame = 0; frame < stub_interface::MAX_FRAMES; frame++) {
        EXPECT_EQ(sent_payload_len(lp.link, frame), frame % 2 == 0 ? mss : PEER_MSS - mss);
    }

    lp.link.clear_frames();
    EXPECT_EQ(output(n.conn.ptr()), OK);
    EXPECT_EQ(lp.link.frames_sent(), 4u);
    EXPECT_EQ(n.conn->sent.count(), 20u);
    EXPECT_EQ(n.conn->sent.lost_bytes(), 0u);
    for (size_t frame = 0; frame < 4; frame++) {
        EXPECT_EQ(sent_payload_len(lp.link, frame), frame % 2 == 0 ? mss : PEER_MSS - mss);
    }
}

TEST(tcp_pmtu, a_narrower_path_is_not_congestion_so_the_window_stays) {
    linked_peer lp;
    narrowed n(lp);
    n.write(2 * PEER_MSS);
    uint32_t cwnd = n.conn->cwnd;
    uint32_t ssthresh = n.conn->ssthresh;

    EXPECT_EQ(n.fragmentation_needed(1000, n.conn->snd_una), OK);
    EXPECT_EQ(n.conn->recovery, recovery_state::loss);
    EXPECT_EQ(n.conn->cwnd, cwnd);
    EXPECT_EQ(n.conn->ssthresh, ssthresh);
    EXPECT_EQ(n.conn->high_seq, n.conn->snd_nxt);
    EXPECT_EQ(n.conn->frto_step, 0);

    EXPECT_EQ(n.ack(n.conn->snd_nxt), OK);
    EXPECT_EQ(n.conn->recovery, recovery_state::open);
    EXPECT_EQ(n.conn->cwnd, cwnd);
}

TEST(tcp_pmtu, a_timeout_after_the_path_narrowed_still_begins_a_congestion_episode) {
    linked_peer lp;
    narrowed n(lp);
    n.write(2 * PEER_MSS);
    EXPECT_EQ(n.fragmentation_needed(1000, n.conn->snd_una), OK);
    ASSERT_EQ(n.conn->recovery, recovery_state::loss);
    uint32_t cwnd = n.conn->cwnd;

    n.fire_rto();
    EXPECT_EQ(n.conn->ssthresh, cwnd / 2);
    EXPECT_EQ(n.conn->cwnd, n.conn->snd_mss);
    EXPECT_EQ(n.conn->prior_cwnd, cwnd);
}

TEST(tcp_pmtu, records_that_fit_the_narrower_path_stay_as_they_are) {
    linked_peer lp;
    narrowed n(lp);
    n.write(500);
    lp.link.clear_frames();

    EXPECT_EQ(n.fragmentation_needed(1000, n.conn->snd_una), OK);
    EXPECT_EQ(n.conn->snd_mss, 1000 - HEADERS);
    EXPECT_EQ(lp.link.frames_sent(), 0u);
    EXPECT_EQ(n.conn->sent.lost_bytes(), 0u);
    EXPECT_EQ(n.conn->recovery, recovery_state::open);
}

TEST(tcp_pmtu, data_written_afterwards_is_segmented_for_the_path) {
    linked_peer lp;
    narrowed n(lp);
    n.write(PEER_MSS);
    EXPECT_EQ(n.fragmentation_needed(1000, n.conn->snd_una), OK);
    lp.link.clear_frames();
    uint32_t mss = 1000 - HEADERS;

    n.write(2 * mss);
    ASSERT_EQ(lp.link.frames_sent(), 2u);
    EXPECT_EQ(sent_payload_len(lp.link, 0), mss);
    EXPECT_EQ(sent_payload_len(lp.link, 1), mss);
}

TEST(tcp_pmtu, a_router_announcing_no_mtu_moves_the_path_down_the_plateaus) {
    linked_peer lp;
    narrowed n(lp);
    n.write(PEER_MSS);

    EXPECT_EQ(n.fragmentation_needed(0, n.conn->snd_una), OK);
    EXPECT_EQ(n.conn->path_mtu, 1492);
    EXPECT_EQ(n.conn->snd_mss, 1492 - HEADERS);

    EXPECT_EQ(n.fragmentation_needed(0, n.conn->snd_una), OK);
    EXPECT_EQ(n.conn->path_mtu, 1006);
    EXPECT_EQ(n.conn->snd_mss, 1006 - HEADERS);
}

TEST(tcp_pmtu, the_path_never_narrows_under_the_floor) {
    linked_peer lp;
    narrowed n(lp);
    n.write(PEER_MSS);

    EXPECT_EQ(n.fragmentation_needed(200, n.conn->snd_una), OK);
    EXPECT_EQ(n.conn->path_mtu, MIN_PATH_MTU);
    EXPECT_EQ(n.conn->snd_mss, MIN_PATH_MTU - HEADERS);
}

TEST(tcp_pmtu, an_mtu_no_narrower_than_the_path_changes_nothing) {
    linked_peer lp;
    narrowed n(lp);
    n.write(PEER_MSS);
    EXPECT_EQ(n.fragmentation_needed(1000, n.conn->snd_una), OK);
    lp.link.clear_frames();

    EXPECT_EQ(n.fragmentation_needed(1200, n.conn->snd_una), OK);
    EXPECT_EQ(n.fragmentation_needed(LINK_MTU, n.conn->snd_una), OK);
    EXPECT_EQ(n.conn->path_mtu, 1000);
    EXPECT_EQ(n.conn->snd_mss, 1000 - HEADERS);
    EXPECT_EQ(lp.link.frames_sent(), 0u);
}

TEST(tcp_pmtu, a_message_about_nothing_outstanding_changes_nothing) {
    linked_peer lp;
    narrowed n(lp);
    n.write(PEER_MSS);
    lp.link.clear_frames();

    EXPECT_EQ(n.fragmentation_needed(1000, n.conn->snd_nxt), OK);
    EXPECT_EQ(n.conn->path_mtu, 0);
    EXPECT_EQ(n.conn->snd_mss, PEER_MSS);
    EXPECT_EQ(lp.link.frames_sent(), 0u);
}

TEST(tcp_pmtu, the_peer_the_socket_and_the_path_each_bound_the_mss) {
    linked_peer lp;
    narrowed n(lp);

    EXPECT_EQ(send_mss(n.conn->iface, PEER_MSS, 0, 0), PEER_MSS);
    EXPECT_EQ(send_mss(n.conn->iface, PEER_MSS, 0, 1000), 1000 - HEADERS);
    EXPECT_EQ(send_mss(n.conn->iface, PEER_MSS, 800, 1000), 800);
    EXPECT_EQ(send_mss(n.conn->iface, 500, 800, 1000), 500);
}
