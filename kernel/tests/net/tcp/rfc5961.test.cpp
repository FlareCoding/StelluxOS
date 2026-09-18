#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "harness.h"
#include "net/tcp/conn.h"
#include "net/tcp/input.h"
#include "net/tcp/seq.h"
#include "resource/resource.h"
#include "clock/clock.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(tcp_rfc5961);

using namespace net;
using namespace net::tcp;

constexpr uint32_t PEER_ISS  = 7000;
constexpr uint32_t PEER_TSVAL = 555;

static uint64_t g_fake_now;

static uint64_t fake_clock() {
    return g_fake_now;
}

// A connection this host opened, established with a peer that scales by 3 and
// stamps its segments, so rcv_nxt is PEER_ISS + 1 and snd_nxt is iss + 1
struct established {
    rc::strong_ref<tcp_conn> conn;
    linked_peer&             lp;

    explicit established(linked_peer& link) : lp(link) {
        g_fake_now = clock::now_ns();
        __dbg_test_set_clock(fake_clock);

        tuple key = {lp.remote.host, lp.remote.addr, lp.remote.host_port, lp.remote.port};
        open_active(key, &lp.link, &conn);

        tcp_options opts;
        opts.mss = 1400;
        opts.has_timestamps = true;
        opts.ts_val = PEER_TSVAL;
        opts.window_scale = 3;
        input(lp.remote.segment(FLAG_SYN | FLAG_ACK, PEER_ISS, conn->iss + 1, opts));
        lp.link.clear_frames();
    }

    ~established() {
        abort_connection(conn.ptr());
        __dbg_test_set_clock(nullptr);
    }

    uint32_t rcv_nxt() const { return PEER_ISS + 1; }
    uint32_t snd_nxt() const { return conn->iss + 1; }

    // A segment stamped newer than the handshake, so PAWS does not interfere
    packet* segment(uint8_t flags, uint32_t seq, uint32_t ack, uint16_t window = 65535) const {
        tcp_options opts;
        opts.has_timestamps = true;
        opts.ts_val = PEER_TSVAL + 1;
        packet* pkt = lp.remote.segment(flags, seq, ack, opts);
        tcp_header* hdr = reinterpret_cast<tcp_header*>(pkt->data());
        hdr->window = htons(window);
        hdr->checksum = 0;
        hdr->checksum = htons(compute_checksum(lp.remote.addr, lp.remote.host, hdr, pkt->length()));
        return pkt;
    }
};

static void expect_challenge_ack(const established& c, size_t frame) {
    const tcp_header* hdr = sent_tcp(c.lp.link, frame);
    EXPECT_EQ(hdr->flags, FLAG_ACK);
    EXPECT_EQ(ntohl(hdr->seq), c.snd_nxt());
    EXPECT_EQ(ntohl(hdr->ack), c.rcv_nxt());
}

TEST(tcp_rfc5961, fixture_is_established_and_quiet) {
    linked_peer lp;
    established c(lp);

    ASSERT_TRUE(c.conn);
    EXPECT_EQ(c.conn->state, tcp_state::established);
    EXPECT_EQ(c.conn->rcv_nxt, c.rcv_nxt());
    EXPECT_EQ(c.conn->snd_wscale, 3);
    EXPECT_EQ(c.conn->max_snd_wnd, 65535u);
    EXPECT_EQ(lp.link.frames_sent(), 0u);
}

TEST(tcp_rfc5961, reset_at_the_expected_sequence_resets_the_connection) {
    linked_peer lp;
    established c(lp);

    EXPECT_EQ(input(c.segment(FLAG_RST, c.rcv_nxt(), 0)), OK);
    EXPECT_EQ(lp.link.frames_sent(), 0u);
    EXPECT_EQ(c.conn->state, tcp_state::closed);
    EXPECT_EQ(c.conn->pending_error, resource::ERR_CONNRESET);
    EXPECT_FALSE(lookup(c.conn->key));
}

TEST(tcp_rfc5961, reset_elsewhere_in_the_window_is_challenged_and_outside_it_ignored) {
    linked_peer lp;
    established c(lp);

    EXPECT_EQ(input(c.segment(FLAG_RST, c.rcv_nxt() + 500, 0)), OK);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    expect_challenge_ack(c, 0);
    EXPECT_EQ(c.conn->state, tcp_state::established);

    EXPECT_EQ(input(c.segment(FLAG_RST, c.rcv_nxt() + RCV_BUF_INITIAL, 0)), OK);
    EXPECT_EQ(input(c.segment(FLAG_RST, c.rcv_nxt() - 1, 0)), OK);
    EXPECT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(c.conn->state, tcp_state::established);
}

TEST(tcp_rfc5961, reset_with_an_old_timestamp_still_counts) {
    linked_peer lp;
    established c(lp);

    tcp_options old_stamp;
    old_stamp.has_timestamps = true;
    old_stamp.ts_val = PEER_TSVAL - 100;
    EXPECT_EQ(input(lp.remote.segment(FLAG_RST, c.rcv_nxt(), 0, old_stamp)), OK);
    EXPECT_EQ(c.conn->state, tcp_state::closed);
}

TEST(tcp_rfc5961, syn_in_established_is_challenged_not_obeyed) {
    linked_peer lp;
    established c(lp);

    EXPECT_EQ(input(c.segment(FLAG_SYN, c.rcv_nxt(), 0)), OK);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    expect_challenge_ack(c, 0);
    EXPECT_EQ(c.conn->state, tcp_state::established);
    EXPECT_EQ(c.conn->rcv_nxt, c.rcv_nxt());
}

TEST(tcp_rfc5961, segment_outside_the_window_is_answered_with_an_ack) {
    linked_peer lp;
    established c(lp);

    EXPECT_EQ(input(c.segment(FLAG_ACK, c.rcv_nxt() - 1000, c.snd_nxt())), OK);
    EXPECT_EQ(input(c.segment(FLAG_ACK, c.rcv_nxt() + RCV_BUF_INITIAL, c.snd_nxt())), OK);
    ASSERT_EQ(lp.link.frames_sent(), 2u);
    expect_challenge_ack(c, 0);
    expect_challenge_ack(c, 1);
}

TEST(tcp_rfc5961, ack_outside_what_this_host_could_have_sent_is_challenged) {
    linked_peer lp;
    established c(lp);
    uint32_t low = c.conn->snd_una - c.conn->max_snd_wnd;

    EXPECT_EQ(input(c.segment(FLAG_ACK, c.rcv_nxt(), c.snd_nxt() + 1)), OK);
    EXPECT_EQ(input(c.segment(FLAG_ACK, c.rcv_nxt(), low - 1)), OK);
    ASSERT_EQ(lp.link.frames_sent(), 2u);
    expect_challenge_ack(c, 0);
    expect_challenge_ack(c, 1);

    // The edges of the range and a plain duplicate are accepted in silence
    EXPECT_EQ(input(c.segment(FLAG_ACK, c.rcv_nxt(), c.snd_nxt())), OK);
    EXPECT_EQ(input(c.segment(FLAG_ACK, c.rcv_nxt(), low)), OK);
    EXPECT_EQ(lp.link.frames_sent(), 2u);
    EXPECT_EQ(c.conn->state, tcp_state::established);
}

TEST(tcp_rfc5961, window_update_follows_the_newest_segment_only) {
    linked_peer lp;
    established c(lp);

    EXPECT_EQ(input(c.segment(FLAG_ACK, c.rcv_nxt(), c.snd_nxt(), 1000)), OK);
    EXPECT_EQ(c.conn->snd_wnd, 1000u << 3);
    EXPECT_EQ(c.conn->snd_wl1, c.rcv_nxt());
    EXPECT_EQ(c.conn->snd_wl2, c.snd_nxt());
    EXPECT_EQ(c.conn->max_snd_wnd, 65535u);

    // An older segment, by sequence, does not move the window back
    EXPECT_EQ(input(c.segment(FLAG_ACK, c.rcv_nxt() - 1, c.snd_nxt(), 5)), OK);
    EXPECT_EQ(c.conn->snd_wnd, 1000u << 3);

    EXPECT_EQ(input(c.segment(FLAG_ACK, c.rcv_nxt(), c.snd_nxt(), 65535)), OK);
    EXPECT_EQ(c.conn->snd_wnd, 65535u << 3);
    EXPECT_EQ(c.conn->max_snd_wnd, 65535u << 3);
    EXPECT_EQ(lp.link.frames_sent(), 1u);
}

TEST(tcp_rfc5961, paws_answers_an_old_timestamp_and_records_a_newer_one) {
    linked_peer lp;
    established c(lp);

    tcp_options older;
    older.has_timestamps = true;
    older.ts_val = PEER_TSVAL - 1;
    EXPECT_EQ(input(lp.remote.segment(FLAG_ACK, c.rcv_nxt(), c.snd_nxt(), older)), OK);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    expect_challenge_ack(c, 0);
    EXPECT_EQ(c.conn->ts_recent, PEER_TSVAL);

    tcp_options newer = older;
    newer.ts_val = PEER_TSVAL + 50;
    EXPECT_EQ(input(lp.remote.segment(FLAG_ACK, c.rcv_nxt(), c.snd_nxt(), newer)), OK);
    EXPECT_EQ(c.conn->ts_recent, PEER_TSVAL + 50);
    EXPECT_EQ(lp.link.frames_sent(), 1u);
}

TEST(tcp_rfc5961, challenge_acks_are_limited_per_second_with_a_hidden_limit) {
    linked_peer lp;
    established c(lp);

    for (size_t i = 0; i < 2 * CHALLENGE_ACK_LIMIT; i++) {
        EXPECT_EQ(input(c.segment(FLAG_RST, c.rcv_nxt() + 500, 0)), OK);
    }

    size_t answered = lp.link.frames_sent();
    EXPECT_TRUE(answered >= CHALLENGE_ACK_LIMIT / 2);
    EXPECT_TRUE(answered <= CHALLENGE_ACK_LIMIT);
    EXPECT_EQ(c.conn->state, tcp_state::established);

    // A new second brings a new allowance
    g_fake_now += CHALLENGE_ACK_WINDOW_NS;
    EXPECT_EQ(input(c.segment(FLAG_RST, c.rcv_nxt() + 500, 0)), OK);
    EXPECT_EQ(lp.link.frames_sent(), answered + 1);
}
