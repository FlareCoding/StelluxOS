#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "harness.h"
#include "net/tcp/conn.h"
#include "net/tcp/reassembly.h"
#include "net/tcp/byte_queue.h"
#include "resource/resource.h"
#include "clock/clock.h"
#include "timer/timer.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(tcp_reassembly);

using namespace net;
using namespace net::tcp;

constexpr uint32_t PEER_ISS = 4000;

static uint64_t g_fake_now;
static uint8_t  g_payload[1400];
static uint8_t  g_read[4 * CHUNK_PAYLOAD];

static uint64_t fake_clock() {
    return g_fake_now;
}

static uint8_t pattern(size_t index) {
    return static_cast<uint8_t>(index * 11 + 5);
}

static const uint8_t* patterned(size_t first, size_t len) {
    for (size_t i = 0; i < len; i++) {
        g_payload[i] = pattern(first + i);
    }

    return g_payload;
}

static bool queue_matches_pattern(tcp_conn* conn, size_t len, size_t first = 0) {
    if (conn->rcv_queue.copy_out(0, g_read, len) != len) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        if (g_read[i] != pattern(first + i)) {
            return false;
        }
    }

    return true;
}

// A connection this host opened, established with a peer that permits SACK
// unless told otherwise and scales by nothing; rcv_nxt starts at PEER_ISS + 1
struct reassembling {
    rc::strong_ref<tcp_conn> conn;
    linked_peer&             lp;

    explicit reassembling(linked_peer& link, bool sack = true) : lp(link) {
        g_fake_now = clock::now_ns();
        __dbg_test_set_clock(fake_clock);

        tuple key = {lp.remote.host, lp.remote.addr, lp.remote.host_port, lp.remote.port};
        open_active(key, &lp.link, &conn);

        tcp_options opts;
        opts.mss = 1400;
        opts.sack_permitted = sack;
        input(lp.remote.segment(FLAG_SYN | FLAG_ACK, PEER_ISS, conn->iss + 1, opts));
        lp.link.clear_frames();
    }

    ~reassembling() {
        abort_connection(conn.ptr());
        __dbg_test_set_clock(nullptr);
    }

    uint32_t first_seq() const { return PEER_ISS + 1; }
    uint32_t snd_nxt() const { return conn->iss + 1; }

    // Shrinks the receive queue to one chunk and advertises exactly that
    size_t narrow_window() {
        RUN_ELEVATED({
            sync::irq_lock_guard guard(conn->lock);
            conn->rcv_queue.set_limit(1);
            conn->rcv_adv = first_seq();
            update_receive_window_locked(conn.ptr());
        });
        return conn->rcv_wnd;
    }

    void read_all() {
        RUN_ELEVATED({
            sync::irq_lock_guard guard(conn->lock);
            conn->rcv_queue.consume(conn->rcv_queue.size());
            update_receive_window_locked(conn.ptr());
        });
    }

    // Stream bytes `first` .. `first + len - 1` at their sequence, returning the frames drawn
    size_t send(size_t first, size_t len, uint8_t flags = FLAG_ACK) {
        lp.link.clear_frames();
        input(lp.remote.segment(flags, first_seq() + static_cast<uint32_t>(first), snd_nxt(), {},
                                patterned(first, len), len));
        return lp.link.frames_sent();
    }

    tcp_options last_options() const {
        tcp_options opts;
        parse_options(sent_tcp(lp.link, lp.link.frames_sent() - 1), &opts);
        return opts;
    }

    uint32_t last_ack() const { return ntohl(sent_tcp(lp.link, lp.link.frames_sent() - 1)->ack); }
};

static void expect_block(const tcp_options& opts, size_t index, const reassembling& r, size_t first, size_t end) {
    ASSERT_TRUE(opts.sack_count > index);
    EXPECT_EQ(opts.sack_blocks[index].start, r.first_seq() + static_cast<uint32_t>(first));
    EXPECT_EQ(opts.sack_blocks[index].end, r.first_seq() + static_cast<uint32_t>(end));
}

TEST(tcp_reassembly, a_segment_ahead_waits_and_is_reported_as_a_sack_block) {
    linked_peer lp;
    reassembling r(lp);
    size_t budget_before = budget_in_use();

    EXPECT_EQ(r.send(100, 50), 1u);
    EXPECT_EQ(r.last_ack(), r.first_seq());
    tcp_options opts = r.last_options();
    EXPECT_EQ(opts.sack_count, 1);
    expect_block(opts, 0, r, 100, 150);

    EXPECT_EQ(r.conn->rcv_nxt, r.first_seq());
    EXPECT_EQ(r.conn->rcv_queue.size(), 0u);
    EXPECT_EQ(r.conn->ooo_queue.size(), 1u);
    EXPECT_EQ(r.conn->ooo_bytes, 50u);
    EXPECT_EQ(budget_in_use(), budget_before + PACKET_OBJECT_SIZE);
}

TEST(tcp_reassembly, filling_the_hole_delivers_everything_in_order_at_once) {
    linked_peer lp;
    reassembling r(lp);
    size_t budget_before = budget_in_use();

    EXPECT_EQ(r.send(100, 50), 1u);
    EXPECT_EQ(r.send(0, 100), 1u);

    EXPECT_EQ(r.conn->rcv_nxt, r.first_seq() + 150);
    EXPECT_EQ(r.conn->rcv_queue.size(), 150u);
    EXPECT_TRUE(queue_matches_pattern(r.conn.ptr(), 150));
    EXPECT_EQ(r.conn->ooo_queue.size(), 0u);
    EXPECT_EQ(r.conn->ooo_bytes, 0u);
    EXPECT_EQ(r.last_ack(), r.first_seq() + 150);
    EXPECT_EQ(r.last_options().sack_count, 0);
    EXPECT_EQ(budget_in_use(), budget_before + CHUNK_SIZE);
}

TEST(tcp_reassembly, blocks_are_reported_newest_first) {
    linked_peer lp;
    reassembling r(lp);

    EXPECT_EQ(r.send(100, 50), 1u);
    EXPECT_EQ(r.send(300, 50), 1u);
    tcp_options opts = r.last_options();
    EXPECT_EQ(opts.sack_count, 2);
    expect_block(opts, 0, r, 300, 350);
    expect_block(opts, 1, r, 100, 150);

    EXPECT_EQ(r.send(500, 50), 1u);
    opts = r.last_options();
    EXPECT_EQ(opts.sack_count, 3);
    expect_block(opts, 0, r, 500, 550);
    expect_block(opts, 1, r, 300, 350);
    expect_block(opts, 2, r, 100, 150);

    EXPECT_EQ(r.send(100, 50), 1u);
    opts = r.last_options();
    expect_block(opts, 0, r, 100, 150);
    expect_block(opts, 1, r, 100, 150);
    expect_block(opts, 2, r, 500, 550);
}

TEST(tcp_reassembly, touching_and_overlapping_arrivals_form_one_block_without_duplicating_bytes) {
    linked_peer lp;
    reassembling r(lp);

    EXPECT_EQ(r.send(100, 50), 1u);
    EXPECT_EQ(r.send(150, 50), 1u);
    tcp_options opts = r.last_options();
    EXPECT_EQ(opts.sack_count, 1);
    expect_block(opts, 0, r, 100, 200);

    EXPECT_EQ(r.send(180, 50), 1u);
    opts = r.last_options();
    EXPECT_EQ(opts.sack_count, 1);
    expect_block(opts, 0, r, 100, 230);
    EXPECT_EQ(r.conn->ooo_bytes, 130u);

    EXPECT_EQ(r.send(80, 40), 1u);
    opts = r.last_options();
    expect_block(opts, 0, r, 80, 230);
    EXPECT_EQ(r.conn->ooo_bytes, 150u);

    EXPECT_EQ(r.send(0, 80), 1u);
    EXPECT_EQ(r.conn->rcv_nxt, r.first_seq() + 230);
    EXPECT_TRUE(queue_matches_pattern(r.conn.ptr(), 230));
    EXPECT_EQ(r.conn->ooo_queue.size(), 0u);
}

TEST(tcp_reassembly, a_duplicate_is_reported_with_a_dsack_block_once) {
    linked_peer lp;
    reassembling r(lp);

    EXPECT_EQ(r.send(0, 100), 1u);
    EXPECT_EQ(r.send(200, 50), 1u);

    EXPECT_EQ(r.send(20, 30), 1u);
    tcp_options opts = r.last_options();
    EXPECT_EQ(opts.sack_count, 2);
    expect_block(opts, 0, r, 20, 50);
    expect_block(opts, 1, r, 200, 250);
    EXPECT_EQ(r.conn->rcv_queue.size(), 100u);

    EXPECT_EQ(r.send(200, 50), 1u);
    opts = r.last_options();
    EXPECT_EQ(opts.sack_count, 2);
    expect_block(opts, 0, r, 200, 250);
    expect_block(opts, 1, r, 200, 250);

    EXPECT_EQ(r.send(100, 100), 1u);
    opts = r.last_options();
    EXPECT_EQ(opts.sack_count, 0);
    EXPECT_EQ(r.conn->rcv_nxt, r.first_seq() + 250);
    EXPECT_TRUE(queue_matches_pattern(r.conn.ptr(), 250));
}

TEST(tcp_reassembly, a_segment_straddling_rcv_nxt_gives_its_new_bytes_and_a_dsack_for_the_old) {
    linked_peer lp;
    reassembling r(lp);

    EXPECT_EQ(r.send(0, 100), 1u);
    EXPECT_EQ(r.send(60, 80), 1u);

    EXPECT_EQ(r.conn->rcv_nxt, r.first_seq() + 140);
    EXPECT_TRUE(queue_matches_pattern(r.conn.ptr(), 140));
    tcp_options opts = r.last_options();
    EXPECT_EQ(opts.sack_count, 1);
    expect_block(opts, 0, r, 60, 100);
}

TEST(tcp_reassembly, a_fin_ahead_waits_with_its_segment_and_closes_when_the_hole_fills) {
    linked_peer lp;
    reassembling r(lp);

    EXPECT_EQ(r.send(100, 20, FLAG_ACK | FLAG_FIN), 1u);
    EXPECT_EQ(r.conn->state, tcp_state::established);
    EXPECT_FALSE(r.conn->fin_rcvd);
    expect_block(r.last_options(), 0, r, 100, 121);

    EXPECT_EQ(r.send(0, 100), 1u);
    EXPECT_EQ(r.conn->state, tcp_state::close_wait);
    EXPECT_TRUE(r.conn->fin_rcvd);
    EXPECT_EQ(r.conn->rcv_nxt, r.first_seq() + 121);
    EXPECT_EQ(r.last_ack(), r.first_seq() + 121);
    EXPECT_TRUE(queue_matches_pattern(r.conn.ptr(), 120));
}

TEST(tcp_reassembly, the_queue_is_bounded_in_packets_and_in_bytes) {
    linked_peer lp;
    reassembling r(lp);

    for (size_t i = 0; i < MAX_OOO_PACKETS; i++) {
        EXPECT_EQ(r.send(10 + i * 20, 10), 1u);
    }

    EXPECT_EQ(r.conn->ooo_queue.size(), MAX_OOO_PACKETS);
    EXPECT_EQ(r.send(10 + MAX_OOO_PACKETS * 20, 10), 1u);
    EXPECT_EQ(r.conn->ooo_queue.size(), MAX_OOO_PACKETS);
    EXPECT_EQ(r.last_options().sack_blocks[0].start, r.first_seq() + 10 + (MAX_OOO_PACKETS - 1) * 20);
}

TEST(tcp_reassembly, queued_bytes_never_exceed_the_room_the_receive_queue_has) {
    linked_peer lp;
    reassembling r(lp);
    r.narrow_window();

    EXPECT_EQ(r.send(100, 1000), 1u);
    EXPECT_EQ(r.conn->ooo_bytes, 1000u);
    EXPECT_EQ(r.send(1200, 1400), 1u);
    EXPECT_EQ(r.conn->ooo_bytes, 1000u);
    EXPECT_EQ(r.conn->rcv_wnd, CHUNK_PAYLOAD - 1000);
}

TEST(tcp_reassembly, bytes_past_the_window_are_dropped_and_so_is_a_fin_behind_them) {
    linked_peer lp;
    reassembling r(lp);
    size_t window = r.narrow_window();

    EXPECT_EQ(r.send(window - 50, 100, FLAG_ACK | FLAG_FIN), 1u);
    EXPECT_EQ(r.conn->ooo_bytes, 50u);
    expect_block(r.last_options(), 0, r, window - 50, window);

    EXPECT_EQ(r.send(0, 1000), 1u);
    EXPECT_EQ(r.send(1000, window - 1050), 1u);
    EXPECT_EQ(r.conn->rcv_nxt, r.first_seq() + window);
    EXPECT_EQ(r.conn->state, tcp_state::established);
    EXPECT_FALSE(r.conn->fin_rcvd);
    EXPECT_TRUE(queue_matches_pattern(r.conn.ptr(), window));

    r.read_all();
    EXPECT_EQ(r.send(window, 50, FLAG_ACK | FLAG_FIN), 1u);
    EXPECT_EQ(r.conn->state, tcp_state::close_wait);
    EXPECT_EQ(r.conn->rcv_nxt, r.first_seq() + window + 51);
    EXPECT_TRUE(queue_matches_pattern(r.conn.ptr(), 50, window));
}

TEST(tcp_reassembly, a_fin_right_behind_the_window_edge_is_kept) {
    linked_peer lp;
    reassembling r(lp);
    size_t window = r.narrow_window();

    EXPECT_EQ(r.send(window - 50, 50, FLAG_ACK | FLAG_FIN), 1u);
    EXPECT_EQ(r.conn->ooo_bytes, 50u);
    expect_block(r.last_options(), 0, r, window - 50, window + 1);

    EXPECT_EQ(r.send(0, 1000), 1u);
    EXPECT_EQ(r.send(1000, window - 1050), 1u);
    EXPECT_EQ(r.conn->state, tcp_state::close_wait);
    EXPECT_EQ(r.conn->rcv_nxt, r.first_seq() + window + 1);
    EXPECT_TRUE(queue_matches_pattern(r.conn.ptr(), window));
}

TEST(tcp_reassembly, a_fin_ahead_without_payload_is_acknowledged_at_once) {
    linked_peer lp;
    reassembling r(lp);
    RUN_ELEVATED({
        sync::irq_lock_guard guard(r.conn->lock);
        r.conn->quick_acks = 0;
    });

    EXPECT_EQ(r.send(100, 0, FLAG_ACK | FLAG_FIN), 1u);
    EXPECT_EQ(r.last_ack(), r.first_seq());
    expect_block(r.last_options(), 0, r, 100, 101);
    EXPECT_FALSE(r.conn->ack_timer_armed);
}

TEST(tcp_reassembly, without_sack_the_queue_works_and_acks_carry_no_blocks) {
    linked_peer lp;
    reassembling r(lp, false);

    EXPECT_EQ(r.send(100, 50), 1u);
    EXPECT_EQ(r.last_options().sack_count, 0);
    EXPECT_EQ(r.conn->ooo_bytes, 50u);

    EXPECT_EQ(r.send(0, 100), 1u);
    EXPECT_EQ(r.conn->rcv_nxt, r.first_seq() + 150);
    EXPECT_TRUE(queue_matches_pattern(r.conn.ptr(), 150));
}

TEST(tcp_reassembly, segments_in_any_order_leave_the_stream_intact) {
    linked_peer lp;
    reassembling r(lp);
    static const size_t order[] = {7, 2, 9, 0, 5, 1, 8, 3, 6, 4};

    for (size_t i = 0; i < 10; i++) {
        r.send(order[i] * 300, 300);
    }

    EXPECT_EQ(r.conn->rcv_nxt, r.first_seq() + 3000);
    EXPECT_EQ(r.conn->rcv_queue.size(), 3000u);
    EXPECT_TRUE(queue_matches_pattern(r.conn.ptr(), 3000));
    EXPECT_EQ(r.conn->ooo_queue.size(), 0u);
    EXPECT_EQ(r.conn->recent_sack_count, 0);
}
