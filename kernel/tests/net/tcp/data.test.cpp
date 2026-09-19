#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "harness.h"
#include "net/tcp/conn.h"
#include "net/tcp/byte_queue.h"
#include "resource/resource.h"
#include "clock/clock.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(tcp_data);

using namespace net;
using namespace net::tcp;

constexpr uint32_t PEER_ISS   = 5000;
constexpr size_t   HALF_CHUNK = CHUNK_PAYLOAD / 2; // fits one segment, two fill a chunk exactly

static uint64_t g_fake_now;
static uint8_t  g_payload[CHUNK_PAYLOAD];
static uint8_t  g_read[CHUNK_PAYLOAD];

static uint64_t fake_clock() {
    return g_fake_now;
}

static uint8_t pattern(size_t index) {
    return static_cast<uint8_t>(index * 5 + 1);
}

static const uint8_t* patterned(size_t first, size_t len) {
    for (size_t i = 0; i < len; i++) {
        g_payload[i] = pattern(first + i);
    }

    return g_payload;
}

static bool queue_matches_pattern(tcp_conn* conn, size_t first, size_t len) {
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

// A connection this host opened, established with a peer that scales by
// nothing, so window fields read as bytes; rcv_nxt starts at PEER_ISS + 1
struct receiving {
    rc::strong_ref<tcp_conn> conn;
    linked_peer&             lp;

    explicit receiving(linked_peer& link) : lp(link) {
        g_fake_now = clock::now_ns();
        __dbg_test_set_clock(fake_clock);

        tuple key = {lp.remote.host, lp.remote.addr, lp.remote.host_port, lp.remote.port};
        open_active(key, &lp.link, &conn);
        input(lp.remote.segment(FLAG_SYN | FLAG_ACK, PEER_ISS, conn->iss + 1));
        lp.link.clear_frames();
    }

    ~receiving() {
        abort_connection(conn.ptr());
        __dbg_test_set_clock(nullptr);
    }

    uint32_t first_seq() const { return PEER_ISS + 1; }
    uint32_t snd_nxt() const { return conn->iss + 1; }

    // Payload bytes `first` .. `first + len - 1` of the stream, at their sequence
    int32_t send(size_t first, size_t len, uint8_t flags = FLAG_ACK) {
        return input(lp.remote.segment(flags, first_seq() + static_cast<uint32_t>(first), snd_nxt(), {},
                                       patterned(first, len), len));
    }
};

static void expect_ack(const receiving& r, size_t frame, uint32_t ack, uint32_t window) {
    ASSERT_TRUE(r.lp.link.frames_sent() > frame);
    const tcp_header* hdr = sent_tcp(r.lp.link, frame);
    EXPECT_EQ(hdr->flags, FLAG_ACK);
    EXPECT_EQ(ntohl(hdr->seq), r.snd_nxt());
    EXPECT_EQ(ntohl(hdr->ack), ack);
    EXPECT_EQ(ntohs(hdr->window), window);
}

TEST(tcp_data, in_order_payload_is_queued_acknowledged_and_narrows_the_window) {
    linked_peer lp;
    receiving r(lp);
    EXPECT_EQ(r.conn->rcv_wnd, RCV_WND_INITIAL);
    EXPECT_EQ(r.conn->rcv_adv, r.first_seq() + RCV_WND_INITIAL);

    EXPECT_EQ(r.send(0, 5), OK);

    EXPECT_EQ(r.conn->rcv_nxt, r.first_seq() + 5);
    EXPECT_EQ(r.conn->rcv_queue.size(), 5u);
    EXPECT_TRUE(queue_matches_pattern(r.conn.ptr(), 0, 5));
    EXPECT_EQ(r.conn->rcv_wnd, RCV_WND_INITIAL - 5);
    EXPECT_EQ(r.conn->rcv_adv, r.first_seq() + RCV_WND_INITIAL);
    EXPECT_EQ(lp.link.frames_sent(), 1u);
    expect_ack(r, 0, r.first_seq() + 5, RCV_WND_INITIAL - 5);
    EXPECT_EQ(r.conn->state, tcp_state::established);
}

TEST(tcp_data, bytes_already_received_are_skipped_and_the_rest_queued) {
    linked_peer lp;
    receiving r(lp);

    EXPECT_EQ(r.send(0, 10), OK);
    EXPECT_EQ(r.send(0, 15), OK);

    EXPECT_EQ(r.conn->rcv_nxt, r.first_seq() + 15);
    EXPECT_EQ(r.conn->rcv_queue.size(), 15u);
    EXPECT_TRUE(queue_matches_pattern(r.conn.ptr(), 0, 15));
    EXPECT_EQ(lp.link.frames_sent(), 2u);
    expect_ack(r, 1, r.first_seq() + 15, RCV_WND_INITIAL - 15);

    EXPECT_EQ(r.send(0, 15), OK);
    EXPECT_EQ(r.conn->rcv_queue.size(), 15u);
    expect_ack(r, 2, r.first_seq() + 15, RCV_WND_INITIAL - 15);
}

TEST(tcp_data, payload_ahead_of_rcv_nxt_is_acknowledged_but_not_queued) {
    linked_peer lp;
    receiving r(lp);

    EXPECT_EQ(r.send(100, 20), OK);

    EXPECT_EQ(r.conn->rcv_nxt, r.first_seq());
    EXPECT_EQ(r.conn->rcv_queue.size(), 0u);
    EXPECT_EQ(r.conn->rcv_wnd, RCV_WND_INITIAL);
    expect_ack(r, 0, r.first_seq(), RCV_WND_INITIAL);
}

TEST(tcp_data, a_fin_behind_payload_is_taken_with_it) {
    linked_peer lp;
    receiving r(lp);

    EXPECT_EQ(r.send(0, 4, FLAG_ACK | FLAG_FIN), OK);

    EXPECT_EQ(r.conn->rcv_nxt, r.first_seq() + 5);
    EXPECT_EQ(r.conn->rcv_queue.size(), 4u);
    EXPECT_TRUE(r.conn->fin_rcvd);
    EXPECT_EQ(r.conn->state, tcp_state::close_wait);
    expect_ack(r, 0, r.first_seq() + 5, RCV_WND_INITIAL - 4);
}

TEST(tcp_data, a_full_queue_takes_nothing_more_and_advertises_zero) {
    linked_peer lp;
    receiving r(lp);
    size_t segments = RCV_WND_INITIAL / HALF_CHUNK;

    for (size_t i = 0; i < segments; i++) {
        lp.link.clear_frames();
        EXPECT_EQ(r.send(i * HALF_CHUNK, HALF_CHUNK), OK);
        expect_ack(r, 0, r.first_seq() + static_cast<uint32_t>((i + 1) * HALF_CHUNK),
                   static_cast<uint32_t>(RCV_WND_INITIAL - (i + 1) * HALF_CHUNK));
    }

    EXPECT_EQ(r.conn->rcv_queue.size(), RCV_WND_INITIAL);
    EXPECT_EQ(r.conn->rcv_wnd, 0u);

    lp.link.clear_frames();
    EXPECT_EQ(r.send(RCV_WND_INITIAL, 1), OK);
    EXPECT_EQ(r.conn->rcv_nxt, r.first_seq() + RCV_WND_INITIAL);
    EXPECT_EQ(r.conn->rcv_queue.size(), RCV_WND_INITIAL);
    expect_ack(r, 0, r.first_seq() + RCV_WND_INITIAL, 0);
}

TEST(tcp_data, payload_beyond_the_room_is_taken_up_to_it) {
    linked_peer lp;
    receiving r(lp);
    RUN_ELEVATED({
        sync::irq_lock_guard guard(r.conn->lock);
        r.conn->rcv_queue.set_limit(1);
        r.conn->rcv_adv = r.first_seq();
        update_receive_window_locked(r.conn.ptr());
    });
    EXPECT_EQ(r.conn->rcv_wnd, CHUNK_PAYLOAD);

    EXPECT_EQ(r.send(0, HALF_CHUNK), OK);
    EXPECT_EQ(r.send(HALF_CHUNK, HALF_CHUNK + 100), OK);

    EXPECT_EQ(r.conn->rcv_queue.size(), CHUNK_PAYLOAD);
    EXPECT_EQ(r.conn->rcv_nxt, r.first_seq() + CHUNK_PAYLOAD);
    EXPECT_EQ(r.conn->rcv_wnd, 0u);
    EXPECT_TRUE(queue_matches_pattern(r.conn.ptr(), 0, CHUNK_PAYLOAD));
}

TEST(tcp_data, payload_after_the_peers_fin_is_ignored) {
    linked_peer lp;
    receiving r(lp);
    EXPECT_EQ(r.send(0, 4, FLAG_ACK | FLAG_FIN), OK);
    ASSERT_EQ(r.conn->state, tcp_state::close_wait);

    EXPECT_EQ(r.send(5, 10), OK);

    EXPECT_EQ(r.conn->rcv_nxt, r.first_seq() + 5);
    EXPECT_EQ(r.conn->rcv_queue.size(), 4u);
}

TEST(tcp_data, payload_arriving_after_this_host_closed_is_refused_with_a_reset) {
    linked_peer lp;
    receiving r(lp);
    close_connection(r.conn.ptr());
    ASSERT_EQ(r.conn->state, tcp_state::fin_wait_1);
    lp.link.clear_frames();

    EXPECT_EQ(r.send(0, 7), OK);

    EXPECT_EQ(r.conn->rcv_queue.size(), 0u);
    EXPECT_EQ(r.conn->state, tcp_state::closed);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(sent_tcp(lp.link, 0)->flags, FLAG_RST | FLAG_ACK);
    EXPECT_EQ(ntohl(sent_tcp(lp.link, 0)->seq), r.snd_nxt() + 1);
    EXPECT_EQ(ntohl(sent_tcp(lp.link, 0)->ack), r.first_seq());
}

TEST(tcp_data, the_advertised_right_edge_never_moves_left) {
    linked_peer lp;
    receiving r(lp);
    uint32_t edge = r.first_seq() + RCV_WND_INITIAL;

    for (size_t i = 0; i < 5; i++) {
        EXPECT_EQ(r.send(i * 1000, 1000), OK);
        EXPECT_EQ(r.conn->rcv_nxt + r.conn->rcv_wnd, edge);
        EXPECT_EQ(r.conn->rcv_adv, edge);
    }

    // Consuming frees room one chunk at a time: 3000 bytes release one whole
    // chunk, the rest stays as the used head of the next
    RUN_ELEVATED({
        sync::irq_lock_guard guard(r.conn->lock);
        (void)r.conn->rcv_queue.consume(3000);
        update_receive_window_locked(r.conn.ptr());
    });
    EXPECT_EQ(r.conn->rcv_nxt + r.conn->rcv_wnd, edge + CHUNK_PAYLOAD);

    RUN_ELEVATED({
        sync::irq_lock_guard guard(r.conn->lock);
        r.conn->rcv_queue.set_limit(1);
        update_receive_window_locked(r.conn.ptr());
    });
    EXPECT_EQ(r.conn->rcv_nxt + r.conn->rcv_wnd, edge);
}
