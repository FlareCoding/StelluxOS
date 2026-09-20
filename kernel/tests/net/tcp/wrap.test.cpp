#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "harness.h"
#include "net/tcp/conn.h"
#include "net/tcp/output.h"
#include "net/tcp/timewait.h"
#include "net/tcp/byte_queue.h"
#include "net/tcp/seq.h"
#include "resource/resource.h"
#include "clock/clock.h"
#include "timer/timer.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(tcp_wrap);

using namespace net;
using namespace net::tcp;

constexpr uint32_t BELOW_WRAP  = 0xFFFFFFFFu - 500; // An ISS 500 sequence numbers short of 2^32
constexpr uint16_t PEER_MSS    = 1000;
constexpr uint16_t PEER_WINDOW = 20000;

static uint64_t g_fake_now;
static uint8_t  g_bytes[3 * CHUNK_PAYLOAD];
static uint8_t  g_read[3 * CHUNK_PAYLOAD];

static uint64_t fake_clock() {
    return g_fake_now;
}

static void advance_and_fire(uint64_t ns) {
    g_fake_now += ns;
    RUN_ELEVATED(timer::__dbg_test_fire_expired(g_fake_now));
}

static uint8_t pattern(size_t index) {
    return static_cast<uint8_t>(index * 5 + 1);
}

static const uint8_t* patterned(size_t first, size_t len) {
    for (size_t i = 0; i < len; i++) {
        g_bytes[i] = pattern(first + i);
    }

    return g_bytes;
}

static tuple key_of(const peer& remote) {
    return tuple{remote.host, remote.addr, remote.host_port, remote.port};
}

// A connection whose sequence numbers start where the test says, ours placed
// between the SYN going out and the SYN-ACK acknowledging it
struct wrapping {
    rc::strong_ref<tcp_conn> conn;
    linked_peer&             lp;
    uint32_t                 our_iss;
    uint32_t                 peer_iss;

    wrapping(linked_peer& link, uint32_t ours = BELOW_WRAP, uint32_t theirs = BELOW_WRAP, uint16_t window = PEER_WINDOW)
        : lp(link), our_iss(ours), peer_iss(theirs) {
        g_fake_now = clock::now_ns();
        __dbg_test_set_clock(fake_clock);

        open_active(key_of(lp.remote), &lp.link, &conn);
        RUN_ELEVATED({
            sync::irq_lock_guard guard(conn->lock);
            conn->iss = ours;
            conn->snd_una = ours;
            conn->snd_nxt = ours + 1;
        });

        tcp_options opts;
        opts.mss = PEER_MSS;
        packet* synack = lp.remote.segment(FLAG_SYN | FLAG_ACK, theirs, ours + 1, opts);
        tcp_header* hdr = reinterpret_cast<tcp_header*>(synack->data());
        hdr->window = htons(window);
        hdr->checksum = 0;
        hdr->checksum = htons(compute_checksum(lp.remote.addr, lp.remote.host, hdr, synack->length()));
        input(synack);
        lp.link.clear_frames();
    }

    ~wrapping() {
        abort_connection(conn.ptr());
        if (timewait()) {
            advance_and_fire(TIMEWAIT_LEN_NS);
        }

        __dbg_test_set_clock(nullptr);
    }

    uint32_t first_seq() const { return our_iss + 1; }
    uint32_t rcv_nxt(size_t received = 0) const { return peer_iss + 1 + static_cast<uint32_t>(received); }

    rc::strong_ref<tcp_timewait> timewait() const {
        rc::strong_ref<record> rec = lookup(key_of(lp.remote));
        if (!rec || rec->kind != record_kind::timewait) {
            return {};
        }

        rec->add_ref();
        return rc::strong_ref<tcp_timewait>::adopt(static_cast<tcp_timewait*>(rec.ptr()));
    }

    size_t write(size_t first, size_t len) {
        size_t queued = 0;
        RUN_ELEVATED({
            sync::irq_lock_guard guard(conn->lock);
            queued = conn->snd_queue.append(patterned(first, len), len);
        });
        (void)output(conn.ptr());
        return queued;
    }

    // A peer segment with `flags` at `seq` acknowledging our stream below `first`, advertising `window`
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

    // The peer's stream bytes `first` .. `first + len - 1` at their sequence, returning the frames drawn
    size_t send(size_t first, size_t len, uint16_t window = 65535) {
        lp.link.clear_frames();
        packet* pkt = lp.remote.segment(FLAG_ACK, rcv_nxt(first), first_seq(), {}, patterned(first, len), len);
        tcp_header* hdr = reinterpret_cast<tcp_header*>(pkt->data());
        hdr->window = htons(window);
        hdr->checksum = 0;
        hdr->checksum = htons(compute_checksum(lp.remote.addr, lp.remote.host, hdr, pkt->length()));
        input(pkt);
        return lp.link.frames_sent();
    }

    bool queue_matches_pattern(size_t len) {
        if (conn->rcv_queue.copy_out(0, g_read, len) != len) {
            return false;
        }

        for (size_t i = 0; i < len; i++) {
            if (g_read[i] != pattern(i)) {
                return false;
            }
        }

        return true;
    }
};

// Frame `frame` carries our stream bytes `first` .. `first + len - 1` with `flags`
static void expect_data(const wrapping& w, size_t frame, uint8_t flags, size_t first, size_t len) {
    ASSERT_TRUE(w.lp.link.frames_sent() > frame);
    const tcp_header* hdr = sent_tcp(w.lp.link, frame);
    EXPECT_EQ(hdr->flags, flags);
    EXPECT_EQ(ntohl(hdr->seq), w.first_seq() + static_cast<uint32_t>(first));
    EXPECT_EQ(w.lp.link.frame_len(frame), eth::HEADER_LEN + ipv4::HEADER_LEN + hdr->header_len() + len);

    const uint8_t* payload = reinterpret_cast<const uint8_t*>(hdr) + hdr->header_len();
    size_t captured = w.lp.link.frame_len(frame) < stub_interface::FRAME_CAPTURE_LEN
                          ? w.lp.link.frame_len(frame) : stub_interface::FRAME_CAPTURE_LEN;
    size_t payload_offset = eth::HEADER_LEN + ipv4::HEADER_LEN + hdr->header_len();
    for (size_t i = 0; payload_offset + i < captured && i < len; i++) {
        EXPECT_EQ(payload[i], pattern(first + i));
    }
}

static void expect_ack(const wrapping& w, size_t frame, uint32_t ack) {
    ASSERT_TRUE(w.lp.link.frames_sent() > frame);
    const tcp_header* hdr = sent_tcp(w.lp.link, frame);
    EXPECT_EQ(hdr->flags, FLAG_ACK);
    EXPECT_EQ(ntohl(hdr->ack), ack);
}

TEST(tcp_wrap, data_segments_and_their_acknowledgments_cross_the_wrap) {
    linked_peer lp;
    wrapping w(lp);

    EXPECT_EQ(w.write(0, 2500), 2500u);

    ASSERT_EQ(lp.link.frames_sent(), 3u);
    expect_data(w, 0, FLAG_ACK, 0, PEER_MSS);
    expect_data(w, 1, FLAG_ACK, PEER_MSS, PEER_MSS);
    expect_data(w, 2, FLAG_PSH | FLAG_ACK, 2 * PEER_MSS, 500);
    EXPECT_TRUE(ntohl(sent_tcp(lp.link, 0)->seq) > ntohl(sent_tcp(lp.link, 1)->seq));
    EXPECT_TRUE(seq_lt(ntohl(sent_tcp(lp.link, 0)->seq), ntohl(sent_tcp(lp.link, 1)->seq)));
    EXPECT_EQ(w.conn->snd_nxt, w.first_seq() + 2500);
    EXPECT_EQ(unsent_bytes(w.conn.ptr()), 0u);

    EXPECT_EQ(w.ack(PEER_MSS), OK);
    EXPECT_EQ(w.conn->snd_una, w.first_seq() + PEER_MSS);
    EXPECT_EQ(w.conn->sent.count(), 2u);
    EXPECT_EQ(w.conn->snd_queue.size(), 1500u);

    EXPECT_EQ(w.ack(2500), OK);
    EXPECT_EQ(w.conn->snd_una, w.first_seq() + 2500);
    EXPECT_TRUE(w.conn->sent.empty());
    EXPECT_EQ(w.conn->snd_queue.size(), 0u);
    EXPECT_EQ(w.conn->send_timer_kind, timer_kind::none);
}

TEST(tcp_wrap, the_oldest_segment_across_the_wrap_is_rebuilt_from_the_same_bytes) {
    linked_peer lp;
    wrapping w(lp);

    EXPECT_EQ(w.write(0, 2 * PEER_MSS), 2u * PEER_MSS);
    lp.link.clear_frames();

    advance_and_fire(TIMEOUT_INIT_NS);

    ASSERT_EQ(lp.link.frames_sent(), 1u);
    expect_data(w, 0, FLAG_ACK, 0, PEER_MSS);
    EXPECT_EQ(w.conn->retransmits, 1);

    EXPECT_EQ(w.ack(PEER_MSS), OK);
    lp.link.clear_frames();
    advance_and_fire(TIMEOUT_INIT_NS);
    expect_data(w, 0, FLAG_PSH | FLAG_ACK, PEER_MSS, PEER_MSS);
}

TEST(tcp_wrap, received_bytes_across_the_wrap_are_queued_in_order_and_old_ones_refused) {
    linked_peer lp;
    wrapping w(lp);

    EXPECT_EQ(w.send(0, 300), 1u);
    EXPECT_EQ(w.send(300, 600), 1u);
    EXPECT_EQ(w.conn->rcv_nxt, w.rcv_nxt(900));
    EXPECT_TRUE(w.conn->rcv_nxt < w.rcv_nxt(0));
    EXPECT_EQ(w.conn->rcv_queue.size(), 900u);
    EXPECT_TRUE(w.queue_matches_pattern(900));
    expect_ack(w, 0, w.rcv_nxt(900));

    EXPECT_EQ(w.send(0, 300), 1u);
    EXPECT_EQ(w.conn->rcv_queue.size(), 900u);
    expect_ack(w, 0, w.rcv_nxt(900));

    size_t beyond = 900 + w.conn->rcv_wnd + 100;
    EXPECT_EQ(w.send(beyond, 100), 1u);
    EXPECT_EQ(w.conn->rcv_queue.size(), 900u);
    EXPECT_EQ(w.conn->ooo_queue.size(), 0u);
    expect_ack(w, 0, w.rcv_nxt(900));
}

TEST(tcp_wrap, out_of_order_segments_straddling_the_wrap_are_sacked_and_drained) {
    linked_peer lp;
    wrapping w(lp);
    RUN_ELEVATED({
        sync::irq_lock_guard guard(w.conn->lock);
        w.conn->sack_ok = true;
    });

    EXPECT_EQ(w.send(600, 200), 1u);
    EXPECT_EQ(w.send(300, 300), 1u);

    tcp_options opts;
    parse_options(sent_tcp(lp.link, 0), &opts);
    ASSERT_EQ(opts.sack_count, 1);
    EXPECT_EQ(opts.sack_blocks[0].start, w.rcv_nxt(300));
    EXPECT_EQ(opts.sack_blocks[0].end, w.rcv_nxt(800));
    EXPECT_TRUE(opts.sack_blocks[0].start > opts.sack_blocks[0].end);
    EXPECT_EQ(w.conn->ooo_queue.size(), 2u);

    EXPECT_EQ(w.send(0, 300), 1u);
    EXPECT_EQ(w.conn->rcv_nxt, w.rcv_nxt(800));
    EXPECT_EQ(w.conn->rcv_queue.size(), 800u);
    EXPECT_TRUE(w.queue_matches_pattern(800));
    EXPECT_EQ(w.conn->ooo_queue.size(), 0u);
    EXPECT_EQ(w.conn->recent_sack_count, 0);
}

TEST(tcp_wrap, the_send_window_edge_wraps_with_the_sequence_space) {
    linked_peer lp;
    wrapping w(lp, 0xFFFFFFFFu - 200, BELOW_WRAP, 1500);

    EXPECT_EQ(w.write(0, 4000), 4000u);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    expect_data(w, 0, FLAG_ACK, 0, PEER_MSS);
    EXPECT_TRUE(w.conn->snd_una > w.conn->snd_nxt);

    lp.link.clear_frames();
    EXPECT_EQ(w.ack(PEER_MSS, 10000), OK);
    ASSERT_EQ(lp.link.frames_sent(), 3u);
    expect_data(w, 0, FLAG_ACK, PEER_MSS, PEER_MSS);
    expect_data(w, 2, FLAG_PSH | FLAG_ACK, 3 * PEER_MSS, PEER_MSS);
    EXPECT_EQ(unsent_bytes(w.conn.ptr()), 0u);
}

TEST(tcp_wrap, nagle_holds_a_small_segment_behind_one_in_flight_across_the_wrap) {
    linked_peer lp;
    wrapping w(lp, 0xFFFFFFFFu - 20);

    EXPECT_EQ(w.write(0, 50), 50u);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_TRUE(w.conn->snd_sml < w.conn->snd_una);
    EXPECT_TRUE(seq_gt(w.conn->snd_sml, w.conn->snd_una));

    EXPECT_EQ(w.write(50, 50), 50u);
    EXPECT_EQ(lp.link.frames_sent(), 1u);

    EXPECT_EQ(w.ack(50), OK);
    ASSERT_EQ(lp.link.frames_sent(), 2u);
    expect_data(w, 1, FLAG_PSH | FLAG_ACK, 50, 50);
}

TEST(tcp_wrap, a_byte_at_the_last_sequence_number_is_acknowledged_by_zero_and_the_fin_follows) {
    linked_peer lp;
    wrapping w(lp, 0xFFFFFFFEu);
    EXPECT_EQ(w.first_seq(), 0xFFFFFFFFu);

    EXPECT_EQ(w.write(0, 1), 1u);
    EXPECT_EQ(w.conn->snd_nxt, 0u);

    EXPECT_EQ(w.ack(1), OK);
    EXPECT_EQ(w.conn->snd_una, 0u);
    EXPECT_TRUE(w.conn->sent.empty());

    lp.link.clear_frames();
    close_connection(w.conn.ptr());
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(sent_tcp(lp.link, 0)->flags, FLAG_FIN | FLAG_ACK);
    EXPECT_EQ(ntohl(sent_tcp(lp.link, 0)->seq), 0u);
    EXPECT_EQ(w.conn->snd_nxt, 1u);

    EXPECT_EQ(w.ack(2), OK);
    EXPECT_EQ(w.conn->state, tcp_state::fin_wait_2);
}

TEST(tcp_wrap, a_zero_window_probe_sits_one_below_a_snd_una_of_zero) {
    linked_peer lp;
    wrapping w(lp, 0xFFFFFFFFu, BELOW_WRAP, 0);
    EXPECT_EQ(w.conn->snd_una, 0u);

    EXPECT_EQ(w.write(0, 100), 100u);
    EXPECT_EQ(lp.link.frames_sent(), 0u);
    EXPECT_EQ(w.conn->send_timer_kind, timer_kind::probe);

    advance_and_fire(TIMEOUT_INIT_NS);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(sent_tcp(lp.link, 0)->flags, FLAG_ACK);
    EXPECT_EQ(ntohl(sent_tcp(lp.link, 0)->seq), 0xFFFFFFFFu);
    EXPECT_EQ(lp.link.frame_len(0), eth::HEADER_LEN + ipv4::HEADER_LEN + sent_tcp(lp.link, 0)->header_len());
}

TEST(tcp_wrap, the_acceptable_acknowledgment_range_wraps_below_zero) {
    linked_peer lp;
    wrapping w(lp, 100);
    uint32_t lowest = w.conn->snd_una - w.conn->max_snd_wnd;
    EXPECT_TRUE(lowest > w.conn->snd_una);

    EXPECT_EQ(input(lp.remote.ack(w.rcv_nxt(), lowest)), OK);
    EXPECT_EQ(lp.link.frames_sent(), 0u);

    EXPECT_EQ(input(lp.remote.ack(w.rcv_nxt(), lowest - 1)), OK);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    expect_ack(w, 0, w.rcv_nxt());

    EXPECT_EQ(input(lp.remote.ack(w.rcv_nxt(), w.conn->snd_nxt + 1)), OK);
    EXPECT_EQ(lp.link.frames_sent(), 2u);
    EXPECT_EQ(w.conn->state, tcp_state::established);
}

TEST(tcp_wrap, a_window_carried_past_the_wrap_is_not_rolled_back_by_the_segment_behind_it) {
    linked_peer lp;
    wrapping w(lp);

    EXPECT_EQ(w.send(600, 100, 8000), 1u);
    EXPECT_EQ(w.conn->ooo_queue.size(), 1u);
    EXPECT_EQ(w.conn->snd_wnd, 8000u);
    EXPECT_TRUE(w.conn->snd_wl1 < w.rcv_nxt(0));

    EXPECT_EQ(w.send(0, 300, 100), 1u);
    EXPECT_EQ(w.conn->rcv_queue.size(), 300u);
    EXPECT_EQ(w.conn->snd_wnd, 8000u);
}

TEST(tcp_wrap, time_wait_judges_a_syn_across_the_wrap) {
    linked_peer lp;
    wrapping w(lp, BELOW_WRAP, 0xFFFFFFFEu);
    EXPECT_EQ(w.conn->rcv_nxt, 0xFFFFFFFFu);

    close_connection(w.conn.ptr());
    EXPECT_EQ(input(lp.remote.segment(FLAG_FIN | FLAG_ACK, w.rcv_nxt(), w.first_seq() + 1)), OK);
    rc::strong_ref<tcp_timewait> tw = w.timewait();
    ASSERT_TRUE(tw);
    EXPECT_EQ(tw->rcv_nxt, 0u);
    lp.link.clear_frames();

    EXPECT_EQ(input(lp.remote.syn(0xFFFFFF00u)), OK);
    EXPECT_EQ(lp.link.frames_sent(), 0u);
    EXPECT_TRUE(w.timewait());

    EXPECT_EQ(input(lp.remote.syn(0x100u)), OK);
    EXPECT_FALSE(w.timewait());
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(sent_tcp(lp.link, 0)->flags, FLAG_RST | FLAG_ACK);
    EXPECT_EQ(ntohl(sent_tcp(lp.link, 0)->ack), 0x101u);
}
