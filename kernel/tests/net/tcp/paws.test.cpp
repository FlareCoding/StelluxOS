#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "harness.h"
#include "net/tcp/conn.h"
#include "net/tcp/output.h"
#include "net/tcp/info.h"
#include "net/tcp/byte_queue.h"
#include "resource/resource.h"
#include "clock/clock.h"
#include "timer/timer.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(tcp_paws);

using namespace net;
using namespace net::tcp;

constexpr uint32_t PEER_ISS = 6000;
constexpr uint32_t PEER_TS  = 1000;
constexpr uint16_t PEER_MSS = 1000;

static uint64_t g_fake_now;
static uint8_t  g_bytes[2 * PEER_MSS];

static uint64_t fake_clock() {
    return g_fake_now;
}

static void advance_and_fire(uint64_t ns) {
    g_fake_now += ns;
    RUN_ELEVATED(timer::__dbg_test_fire_expired(g_fake_now));
}

static uint8_t pattern(size_t index) {
    return static_cast<uint8_t>(index * 13 + 2);
}

static const uint8_t* patterned(size_t first, size_t len) {
    for (size_t i = 0; i < len; i++) {
        g_bytes[i] = pattern(first + i);
    }

    return g_bytes;
}

static tcp_options sent_options(const stub_interface& link, size_t frame) {
    tcp_options opts;
    parse_options(sent_tcp(link, frame), &opts);
    return opts;
}

// A connection this host opened with timestamps negotiated, the peer's clock
// starting at `first_ts` and rcv_nxt at PEER_ISS + 1
struct stamped {
    rc::strong_ref<tcp_conn> conn;
    linked_peer&             lp;

    explicit stamped(linked_peer& link, uint32_t first_ts = PEER_TS) : lp(link) {
        g_fake_now = clock::now_ns();
        __dbg_test_set_clock(fake_clock);

        tuple key = {lp.remote.host, lp.remote.addr, lp.remote.host_port, lp.remote.port};
        open_active(key, &lp.link, &conn);
        uint32_t syn_ts = sent_options(lp.link, 0).ts_val;

        tcp_options opts;
        opts.mss = PEER_MSS;
        opts.has_timestamps = true;
        opts.ts_val = first_ts;
        opts.ts_ecr = syn_ts;
        input(lp.remote.segment(FLAG_SYN | FLAG_ACK, PEER_ISS, conn->iss + 1, opts));
        lp.link.clear_frames();
    }

    ~stamped() {
        abort_connection(conn.ptr());
        __dbg_test_set_clock(nullptr);
    }

    uint32_t rcv_nxt(size_t received = 0) const { return PEER_ISS + 1 + static_cast<uint32_t>(received); }
    uint32_t snd_nxt() const { return conn->iss + 1; }

    // A peer segment stamped `ts_val` with `len` stream bytes from `first`, returning the frames drawn
    size_t send(uint8_t flags, size_t first, size_t len, uint32_t ts_val) {
        lp.link.clear_frames();
        tcp_options opts;
        opts.has_timestamps = true;
        opts.ts_val = ts_val;
        input(lp.remote.segment(flags, rcv_nxt(first), snd_nxt(), opts, len > 0 ? patterned(first, len) : nullptr, len));
        return lp.link.frames_sent();
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
};

static void expect_ack(const stamped& s, size_t frame, uint32_t ack) {
    ASSERT_TRUE(s.lp.link.frames_sent() > frame);
    const tcp_header* hdr = sent_tcp(s.lp.link, frame);
    EXPECT_EQ(hdr->flags, FLAG_ACK);
    EXPECT_EQ(ntohl(hdr->ack), ack);
}

TEST(tcp_paws, data_with_an_older_timestamp_is_dropped_and_answered_without_being_queued) {
    linked_peer lp;
    stamped s(lp);
    tcp_counters before;
    describe_counters(&before);

    EXPECT_EQ(s.send(FLAG_ACK, 0, 100, PEER_TS + 10), 1u);
    EXPECT_EQ(s.conn->rcv_queue.size(), 100u);
    EXPECT_EQ(s.conn->ts_recent, PEER_TS + 10);

    EXPECT_EQ(s.send(FLAG_ACK, 100, 100, PEER_TS + 5), 1u);
    EXPECT_EQ(s.conn->rcv_queue.size(), 100u);
    EXPECT_EQ(s.conn->rcv_nxt, s.rcv_nxt(100));
    EXPECT_EQ(s.conn->ts_recent, PEER_TS + 10);
    expect_ack(s, 0, s.rcv_nxt(100));

    tcp_counters after;
    describe_counters(&after);
    EXPECT_EQ(after.paws_drops, before.paws_drops + 1);

    EXPECT_EQ(s.send(FLAG_ACK, 100, 100, PEER_TS + 11), 1u);
    EXPECT_EQ(s.conn->rcv_queue.size(), 200u);
    EXPECT_EQ(s.conn->ts_recent, PEER_TS + 11);
}

TEST(tcp_paws, a_fin_with_an_older_timestamp_is_dropped) {
    linked_peer lp;
    stamped s(lp);

    EXPECT_EQ(s.send(FLAG_ACK, 0, 10, PEER_TS + 10), 1u);

    EXPECT_EQ(s.send(FLAG_ACK | FLAG_FIN, 10, 0, PEER_TS + 5), 1u);
    EXPECT_EQ(s.conn->state, tcp_state::established);
    EXPECT_FALSE(s.conn->fin_rcvd);
    EXPECT_EQ(s.conn->rcv_nxt, s.rcv_nxt(10));
    expect_ack(s, 0, s.rcv_nxt(10));

    EXPECT_EQ(s.send(FLAG_ACK | FLAG_FIN, 10, 0, PEER_TS + 12), 1u);
    EXPECT_EQ(s.conn->state, tcp_state::close_wait);
    EXPECT_TRUE(s.conn->fin_rcvd);
    expect_ack(s, 0, s.rcv_nxt(11));
}

TEST(tcp_paws, timestamps_compare_modulo_two_to_the_thirty_two) {
    linked_peer lp;
    stamped s(lp, 0xFFFFFFF0u);
    EXPECT_EQ(s.conn->ts_recent, 0xFFFFFFF0u);

    EXPECT_EQ(s.send(FLAG_ACK, 0, 10, 0x10u), 1u);
    EXPECT_EQ(s.conn->rcv_queue.size(), 10u);
    EXPECT_EQ(s.conn->ts_recent, 0x10u);

    EXPECT_EQ(s.send(FLAG_ACK, 10, 10, 0xFFFFFFF0u), 1u);
    EXPECT_EQ(s.conn->rcv_queue.size(), 10u);
    EXPECT_EQ(s.conn->ts_recent, 0x10u);

    EXPECT_EQ(s.send(FLAG_ACK, 10, 10, 0x11u), 1u);
    EXPECT_EQ(s.conn->rcv_queue.size(), 20u);
}

TEST(tcp_paws, ts_recent_follows_only_segments_at_or_below_rcv_nxt) {
    linked_peer lp;
    stamped s(lp);

    EXPECT_EQ(s.send(FLAG_ACK, 200, 10, PEER_TS + 500), 1u);
    EXPECT_EQ(s.conn->ooo_queue.size(), 1u);
    EXPECT_EQ(s.conn->ts_recent, PEER_TS);

    EXPECT_EQ(s.send(FLAG_ACK, 0, 10, PEER_TS + 1), 1u);
    EXPECT_EQ(s.conn->rcv_queue.size(), 10u);
    EXPECT_EQ(s.conn->ts_recent, PEER_TS + 1);
}

TEST(tcp_paws, an_idle_connection_lets_an_older_timestamp_through_after_the_clock_could_have_wrapped) {
    linked_peer lp;
    stamped s(lp);

    EXPECT_EQ(s.send(FLAG_ACK, 0, 10, PEER_TS + 100), 1u);
    EXPECT_EQ(s.conn->ts_recent, PEER_TS + 100);

    g_fake_now += TS_RECENT_MAX_AGE_NS + 1000000000ULL;

    EXPECT_EQ(s.send(FLAG_ACK, 10, 10, PEER_TS + 50), 1u);
    EXPECT_EQ(s.conn->rcv_queue.size(), 20u);
    EXPECT_EQ(s.conn->ts_recent, PEER_TS + 50);
}

TEST(tcp_paws, our_acknowledgments_echo_the_newest_timestamp_received) {
    linked_peer lp;
    stamped s(lp);

    EXPECT_EQ(s.send(FLAG_ACK, 0, 10, PEER_TS + 7), 1u);
    tcp_options opts = sent_options(lp.link, 0);
    EXPECT_TRUE(opts.has_timestamps);
    EXPECT_EQ(opts.ts_ecr, PEER_TS + 7);
}

TEST(tcp_paws, the_echoed_timestamp_measures_the_round_trip_across_the_wrap) {
    linked_peer lp;
    stamped s(lp);
    EXPECT_EQ(s.conn->srtt_us, 0u);

    uint32_t ticks_now = static_cast<uint32_t>(g_fake_now / TIMESTAMP_TICK_NS);
    RUN_ELEVATED({
        sync::irq_lock_guard guard(s.conn->lock);
        s.conn->ts_offset = 0xFFFFFFF0u - ticks_now;
    });

    EXPECT_EQ(s.write(0, 100), 100u);
    EXPECT_EQ(sent_options(lp.link, 0).ts_val, 0xFFFFFFF0u);

    advance_and_fire(TIMEOUT_INIT_NS);
    ASSERT_EQ(lp.link.frames_sent(), 2u);
    uint32_t echoed = sent_options(lp.link, 1).ts_val;
    EXPECT_EQ(echoed, 0xFFFFFFF0u + 1000);
    EXPECT_TRUE(echoed < 0xFFFFFFF0u);

    g_fake_now += 100 * TIMESTAMP_TICK_NS;
    tcp_options opts;
    opts.has_timestamps = true;
    opts.ts_val = PEER_TS + 1;
    opts.ts_ecr = echoed;
    EXPECT_EQ(input(lp.remote.segment(FLAG_ACK, s.rcv_nxt(), s.snd_nxt() + 100, opts)), OK);

    EXPECT_EQ(s.conn->srtt_us, 100000u);
    EXPECT_TRUE(s.conn->sent.empty());
}
