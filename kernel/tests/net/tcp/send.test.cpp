#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "harness.h"
#include "net/tcp/conn.h"
#include "net/tcp/output.h"
#include "net/tcp/timewait.h"
#include "net/tcp/byte_queue.h"
#include "resource/resource.h"
#include "clock/clock.h"
#include "timer/timer.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(tcp_send);

using namespace net;
using namespace net::tcp;

constexpr uint32_t PEER_ISS    = 8000;
constexpr uint16_t PEER_MSS    = 1000;
constexpr uint16_t PEER_WINDOW = 20000;

static uint64_t g_fake_now;
static uint8_t  g_bytes[3 * CHUNK_PAYLOAD];

static uint64_t fake_clock() {
    return g_fake_now;
}

static void advance_and_fire(uint64_t ns) {
    g_fake_now += ns;
    RUN_ELEVATED(timer::__dbg_test_fire_expired(g_fake_now));
}

static uint8_t pattern(size_t index) {
    return static_cast<uint8_t>(index * 3 + 7);
}

static const uint8_t* patterned(size_t first, size_t len) {
    for (size_t i = 0; i < len; i++) {
        g_bytes[i] = pattern(first + i);
    }

    return g_bytes;
}

// A connection this host opened, established with a peer whose MSS is
// PEER_MSS, whose window is PEER_WINDOW, and that scales by nothing
struct sending {
    rc::strong_ref<tcp_conn> conn;
    linked_peer&             lp;

    explicit sending(linked_peer& link, uint16_t window = PEER_WINDOW) : lp(link) {
        g_fake_now = clock::now_ns();
        __dbg_test_set_clock(fake_clock);

        tuple key = {lp.remote.host, lp.remote.addr, lp.remote.host_port, lp.remote.port};
        open_active(key, &lp.link, &conn);

        tcp_options opts;
        opts.mss = PEER_MSS;
        packet* synack = lp.remote.segment(FLAG_SYN | FLAG_ACK, PEER_ISS, conn->iss + 1, opts);
        tcp_header* hdr = reinterpret_cast<tcp_header*>(synack->data());
        hdr->window = htons(window);
        hdr->checksum = 0;
        hdr->checksum = htons(compute_checksum(lp.remote.addr, lp.remote.host, hdr, synack->length()));
        input(synack);
        lp.link.clear_frames();
    }

    ~sending() {
        abort_connection(conn.ptr());
        __dbg_test_set_clock(nullptr);
    }

    uint32_t first_seq() const { return conn->iss + 1; }
    uint32_t rcv_nxt() const { return PEER_ISS + 1; }

    // Queues stream bytes `first` .. `first + len - 1` and offers them to the link
    size_t write(size_t first, size_t len) {
        size_t queued = 0;
        RUN_ELEVATED({
            sync::irq_lock_guard guard(conn->lock);
            queued = conn->snd_queue.append(patterned(first, len), len);
        });
        (void)output(conn.ptr());
        return queued;
    }

    // A peer segment with `flags` at `seq` acknowledging stream bytes below
    // `first`, advertising `window`
    int32_t peer_segment(uint8_t flags, uint32_t seq, size_t first, uint16_t window) {
        packet* pkt = lp.remote.segment(flags, seq, first_seq() + static_cast<uint32_t>(first));
        tcp_header* hdr = reinterpret_cast<tcp_header*>(pkt->data());
        hdr->window = htons(window);
        hdr->checksum = 0;
        hdr->checksum = htons(compute_checksum(lp.remote.addr, lp.remote.host, hdr, pkt->length()));
        return input(pkt);
    }

    int32_t ack(size_t first, uint16_t window = PEER_WINDOW) {
        return peer_segment(FLAG_ACK, rcv_nxt(), first, window);
    }
};

// Frame `frame` carries stream bytes `first` .. `first + len - 1` with `flags`
static void expect_data(const sending& s, size_t frame, uint8_t flags, size_t first, size_t len) {
    ASSERT_TRUE(s.lp.link.frames_sent() > frame);
    const tcp_header* hdr = sent_tcp(s.lp.link, frame);
    EXPECT_EQ(hdr->flags, flags);
    EXPECT_EQ(ntohl(hdr->seq), s.first_seq() + static_cast<uint32_t>(first));
    EXPECT_EQ(ntohl(hdr->ack), s.rcv_nxt());
    EXPECT_EQ(s.lp.link.frame_len(frame), eth::HEADER_LEN + ipv4::HEADER_LEN + hdr->header_len() + len);

    const uint8_t* payload = reinterpret_cast<const uint8_t*>(hdr) + hdr->header_len();
    size_t captured = s.lp.link.frame_len(frame) < stub_interface::FRAME_CAPTURE_LEN
                          ? s.lp.link.frame_len(frame) : stub_interface::FRAME_CAPTURE_LEN;
    size_t payload_offset = eth::HEADER_LEN + ipv4::HEADER_LEN + hdr->header_len();
    for (size_t i = 0; payload_offset + i < captured && i < len; i++) {
        EXPECT_EQ(payload[i], pattern(first + i));
    }
}

TEST(tcp_send, a_write_goes_out_as_one_pushed_segment_and_is_acknowledged) {
    linked_peer lp;
    sending s(lp);
    EXPECT_EQ(s.conn->cwnd, 10u * PEER_MSS);
    EXPECT_EQ(s.conn->snd_wnd, PEER_WINDOW);

    EXPECT_EQ(s.write(0, 100), 100u);

    EXPECT_EQ(lp.link.frames_sent(), 1u);
    expect_data(s, 0, FLAG_PSH | FLAG_ACK, 0, 100);
    EXPECT_EQ(s.conn->snd_nxt, s.first_seq() + 100);
    EXPECT_EQ(s.conn->snd_una, s.first_seq());
    EXPECT_EQ(s.conn->sent.count(), 1u);
    EXPECT_EQ(s.conn->send_timer_kind, timer_kind::rto);
    EXPECT_EQ(unsent_bytes(s.conn.ptr()), 0u);

    EXPECT_EQ(s.ack(100), OK);
    EXPECT_EQ(s.conn->snd_una, s.first_seq() + 100);
    EXPECT_TRUE(s.conn->sent.empty());
    EXPECT_EQ(s.conn->snd_queue.size(), 0u);
    EXPECT_EQ(s.conn->send_timer_kind, timer_kind::none);
    EXPECT_EQ(lp.link.frames_sent(), 1u);
}

TEST(tcp_send, a_long_write_is_cut_into_full_segments_with_push_on_the_last) {
    linked_peer lp;
    sending s(lp);

    EXPECT_EQ(s.write(0, 2500), 2500u);

    ASSERT_EQ(lp.link.frames_sent(), 3u);
    expect_data(s, 0, FLAG_ACK, 0, PEER_MSS);
    expect_data(s, 1, FLAG_ACK, PEER_MSS, PEER_MSS);
    expect_data(s, 2, FLAG_PSH | FLAG_ACK, 2 * PEER_MSS, 500);
    EXPECT_EQ(s.conn->sent.count(), 3u);
    EXPECT_EQ(s.conn->snd_nxt, s.first_seq() + 2500);
}

TEST(tcp_send, the_congestion_window_caps_what_is_in_flight_until_acknowledgments_arrive) {
    linked_peer lp;
    sending s(lp);
    size_t half = 6 * PEER_MSS;
    size_t total = 2 * half;

    EXPECT_EQ(s.write(0, half), half);
    EXPECT_EQ(s.write(half, half), half);
    EXPECT_EQ(lp.link.frames_sent(), 10u);
    EXPECT_EQ(s.conn->snd_nxt, s.first_seq() + 10 * PEER_MSS);
    EXPECT_EQ(unsent_bytes(s.conn.ptr()), total - 10 * PEER_MSS);

    lp.link.clear_frames();
    EXPECT_EQ(s.ack(2 * PEER_MSS), OK);
    ASSERT_EQ(lp.link.frames_sent(), 2u);
    expect_data(s, 0, FLAG_ACK, 10 * PEER_MSS, PEER_MSS);
    expect_data(s, 1, FLAG_PSH | FLAG_ACK, 11 * PEER_MSS, PEER_MSS);
    EXPECT_EQ(s.conn->snd_queue.size(), total - 2 * PEER_MSS);
    EXPECT_EQ(unsent_bytes(s.conn.ptr()), 0u);
}

TEST(tcp_send, the_peers_window_caps_what_is_sent_until_it_opens) {
    linked_peer lp;
    sending s(lp, 1500);

    EXPECT_EQ(s.write(0, 4000), 4000u);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    expect_data(s, 0, FLAG_ACK, 0, PEER_MSS);
    EXPECT_EQ(s.conn->snd_nxt, s.first_seq() + PEER_MSS);

    lp.link.clear_frames();
    EXPECT_EQ(s.ack(PEER_MSS, 0), OK);
    EXPECT_EQ(lp.link.frames_sent(), 0u);

    EXPECT_EQ(s.ack(PEER_MSS, 10000), OK);
    ASSERT_EQ(lp.link.frames_sent(), 3u);
    expect_data(s, 0, FLAG_ACK, PEER_MSS, PEER_MSS);
    expect_data(s, 2, FLAG_PSH | FLAG_ACK, 3 * PEER_MSS, PEER_MSS);
    EXPECT_EQ(unsent_bytes(s.conn.ptr()), 0u);
}

TEST(tcp_send, a_small_write_waits_behind_an_unacknowledged_small_segment_unless_nodelay) {
    linked_peer lp;
    sending s(lp);

    EXPECT_EQ(s.write(0, 10), 10u);
    EXPECT_EQ(lp.link.frames_sent(), 1u);

    EXPECT_EQ(s.write(10, 10), 10u);
    EXPECT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(unsent_bytes(s.conn.ptr()), 10u);

    EXPECT_EQ(s.ack(10), OK);
    ASSERT_EQ(lp.link.frames_sent(), 2u);
    expect_data(s, 1, FLAG_PSH | FLAG_ACK, 10, 10);

    s.conn->nodelay = true;
    EXPECT_EQ(s.write(20, 10), 10u);
    ASSERT_EQ(lp.link.frames_sent(), 3u);
    expect_data(s, 2, FLAG_PSH | FLAG_ACK, 20, 10);
}

TEST(tcp_send, a_small_write_behind_full_segments_goes_at_once) {
    linked_peer lp;
    sending s(lp);

    EXPECT_EQ(s.write(0, 2 * PEER_MSS), 2u * PEER_MSS);
    ASSERT_EQ(lp.link.frames_sent(), 2u);

    EXPECT_EQ(s.write(2 * PEER_MSS, 10), 10u);
    ASSERT_EQ(lp.link.frames_sent(), 3u);
    expect_data(s, 2, FLAG_PSH | FLAG_ACK, 2 * PEER_MSS, 10);

    EXPECT_EQ(s.write(2 * PEER_MSS + 10, 10), 10u);
    EXPECT_EQ(lp.link.frames_sent(), 3u);
}

TEST(tcp_send, a_partial_segment_is_held_back_from_a_small_window_while_data_is_in_flight) {
    linked_peer lp;
    sending s(lp);

    EXPECT_EQ(s.write(0, 2 * PEER_MSS), 2u * PEER_MSS);
    ASSERT_EQ(lp.link.frames_sent(), 2u);

    EXPECT_EQ(s.ack(PEER_MSS, 1300), OK);
    EXPECT_EQ(s.write(2 * PEER_MSS, 200), 200u);
    EXPECT_EQ(lp.link.frames_sent(), 2u);

    EXPECT_EQ(s.ack(2 * PEER_MSS, 1300), OK);
    ASSERT_EQ(lp.link.frames_sent(), 3u);
    expect_data(s, 2, FLAG_PSH | FLAG_ACK, 2 * PEER_MSS, 200);
}

TEST(tcp_send, the_queue_stops_taking_bytes_when_full_and_resumes_as_acknowledgments_free_it) {
    linked_peer lp;
    sending s(lp);
    size_t capacity = SND_CHUNKS_INITIAL * CHUNK_PAYLOAD;

    size_t queued = 0;
    while (queued < capacity) {
        size_t want = capacity - queued < sizeof(g_bytes) ? capacity - queued : sizeof(g_bytes);
        queued += s.write(queued, want);
    }

    EXPECT_EQ(s.conn->snd_queue.size(), capacity);
    EXPECT_EQ(s.write(capacity, 1), 0u);
    EXPECT_EQ(s.conn->snd_queue.free_space(), 0u);

    EXPECT_EQ(s.ack(CHUNK_PAYLOAD), OK);
    EXPECT_EQ(s.conn->snd_queue.free_space(), CHUNK_PAYLOAD);
    EXPECT_EQ(s.write(capacity, 1), 1u);
}

TEST(tcp_send, the_oldest_segment_is_sent_again_when_the_timer_fires) {
    linked_peer lp;
    sending s(lp);

    EXPECT_EQ(s.write(0, 2 * PEER_MSS), 2u * PEER_MSS);
    ASSERT_EQ(lp.link.frames_sent(), 2u);
    lp.link.clear_frames();

    advance_and_fire(TIMEOUT_INIT_NS);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    expect_data(s, 0, FLAG_ACK, 0, PEER_MSS);
    EXPECT_EQ(s.conn->sent.oldest()->retrans, 1);
    EXPECT_EQ(s.conn->retransmits, 1);
    EXPECT_EQ(s.conn->send_timer_deadline_ns, g_fake_now + 2 * TIMEOUT_INIT_NS);

    EXPECT_EQ(s.ack(PEER_MSS), OK);
    EXPECT_EQ(s.conn->retransmits, 0);
    EXPECT_EQ(s.conn->sent.count(), 1u);
    EXPECT_EQ(s.conn->send_timer_kind, timer_kind::rto);

    lp.link.clear_frames();
    advance_and_fire(TIMEOUT_INIT_NS);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    expect_data(s, 0, FLAG_PSH | FLAG_ACK, PEER_MSS, PEER_MSS);
}

TEST(tcp_send, unacknowledged_data_ends_in_a_reset_after_the_retries) {
    linked_peer lp;
    sending s(lp);

    EXPECT_EQ(s.write(0, 100), 100u);
    for (int i = 0; i < DATA_RETRIES; i++) {
        advance_and_fire(TIMEOUT_MAX_NS);
        EXPECT_EQ(s.conn->state, tcp_state::established);
    }

    lp.link.clear_frames();
    advance_and_fire(TIMEOUT_MAX_NS);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(sent_tcp(lp.link, 0)->flags, FLAG_RST | FLAG_ACK);
    EXPECT_EQ(s.conn->state, tcp_state::closed);
    EXPECT_EQ(s.conn->pending_error, resource::ERR_TIMEDOUT);
}

TEST(tcp_send, a_close_lets_the_queued_data_out_before_its_fin) {
    linked_peer lp;
    sending s(lp, 1000);

    EXPECT_EQ(s.write(0, 1500), 1500u);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    lp.link.clear_frames();

    close_connection(s.conn.ptr());
    EXPECT_EQ(s.conn->state, tcp_state::fin_wait_1);
    EXPECT_TRUE(s.conn->fin_pending);
    EXPECT_FALSE(s.conn->fin_sent);
    EXPECT_EQ(lp.link.frames_sent(), 0u);

    EXPECT_EQ(s.ack(PEER_MSS, 10000), OK);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    expect_data(s, 0, FLAG_FIN | FLAG_PSH | FLAG_ACK, PEER_MSS, 500);
    EXPECT_TRUE(s.conn->fin_sent);
    EXPECT_FALSE(s.conn->fin_pending);
    EXPECT_EQ(s.conn->snd_nxt, s.first_seq() + 1501);

    lp.link.clear_frames();
    advance_and_fire(TIMEOUT_INIT_NS);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    expect_data(s, 0, FLAG_FIN | FLAG_PSH | FLAG_ACK, PEER_MSS, 500);

    EXPECT_EQ(s.ack(1501), OK);
    EXPECT_EQ(s.conn->state, tcp_state::fin_wait_2);
    EXPECT_TRUE(s.conn->sent.empty());
}

TEST(tcp_send, a_close_sends_held_small_data_at_once_with_the_fin) {
    linked_peer lp;
    sending s(lp);

    EXPECT_EQ(s.write(0, 10), 10u);
    EXPECT_EQ(s.write(10, 10), 10u);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    lp.link.clear_frames();

    close_connection(s.conn.ptr());
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    expect_data(s, 0, FLAG_FIN | FLAG_PSH | FLAG_ACK, 10, 10);
    EXPECT_TRUE(s.conn->fin_sent);
    EXPECT_EQ(s.conn->snd_nxt, s.first_seq() + 21);
}

TEST(tcp_send, the_peers_fin_arriving_before_our_fin_went_out_does_not_strand_it) {
    linked_peer lp;
    sending s(lp, 1000);

    EXPECT_EQ(s.write(0, 1500), 1500u);
    close_connection(s.conn.ptr());
    ASSERT_EQ(s.conn->state, tcp_state::fin_wait_1);
    lp.link.clear_frames();

    EXPECT_EQ(s.peer_segment(FLAG_FIN | FLAG_ACK, s.rcv_nxt(), 0, 1000), OK);
    EXPECT_EQ(s.conn->state, tcp_state::closing);
    EXPECT_TRUE(s.conn->fin_pending);
    lp.link.clear_frames();

    EXPECT_EQ(s.peer_segment(FLAG_ACK, s.rcv_nxt() + 1, PEER_MSS, 10000), OK);

    ASSERT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(sent_tcp(lp.link, 0)->flags, FLAG_FIN | FLAG_PSH | FLAG_ACK);
    EXPECT_EQ(ntohl(sent_tcp(lp.link, 0)->seq), s.first_seq() + PEER_MSS);
    EXPECT_TRUE(s.conn->fin_sent);
    EXPECT_EQ(s.conn->send_timer_kind, timer_kind::rto);

    EXPECT_EQ(s.peer_segment(FLAG_ACK, s.rcv_nxt() + 1, 1501, 10000), OK);
    EXPECT_EQ(s.conn->state, tcp_state::closed);
    EXPECT_TRUE(lookup(tuple{lp.remote.host, lp.remote.addr, lp.remote.host_port, lp.remote.port}));
    advance_and_fire(TIMEWAIT_LEN_NS);
}
