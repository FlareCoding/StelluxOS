#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "harness.h"
#include "net/tcp/conn.h"
#include "net/tcp/timers.h"
#include "net/tcp/output.h"
#include "net/net.h"
#include "net/eth.h"
#include "net/ipv4.h"
#include "resource/resource.h"
#include "clock/clock.h"
#include "timer/timer.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(tcp_keepalive);

using namespace net;
using namespace net::tcp;

constexpr uint32_t PEER_ISS = 8000;
constexpr uint16_t PEER_MSS = 1000;
constexpr uint64_t SECOND   = 1000000000ULL;

static uint64_t g_fake_now;
static uint8_t  g_bytes[PEER_MSS];

static uint64_t fake_clock() {
    return g_fake_now;
}

// A connection this host opened, with keepalives of 10 s idle, 2 s apart, 3 probes
struct kept_alive {
    rc::strong_ref<tcp_conn> conn;
    linked_peer&             lp;

    explicit kept_alive(linked_peer& link) : lp(link) {
        g_fake_now = clock::now_ns();
        __dbg_test_set_clock(fake_clock);

        tuple key = {lp.remote.host, lp.remote.addr, lp.remote.host_port, lp.remote.port};
        open_active(key, &lp.link, &conn);

        tcp_options opts;
        opts.mss = PEER_MSS;
        input(lp.remote.segment(FLAG_SYN | FLAG_ACK, PEER_ISS, conn->iss + 1, opts));
        lp.link.clear_frames();
    }

    ~kept_alive() {
        abort_connection(conn.ptr());
        __dbg_test_set_clock(nullptr);
    }

    void enable() {
        RUN_ELEVATED({
            sync::irq_lock_guard guard(conn->lock);
            conn->keepalive = true;
            conn->keepalive_idle_s = 10;
            conn->keepalive_interval_s = 2;
            conn->keepalive_probes = 3;
            arm_keepalive_locked(conn.ptr());
        });
    }

    void expire() {
        g_fake_now = conn->keepalive_deadline_ns;
        RUN_ELEVATED(timer::__dbg_test_fire_expired(g_fake_now));
    }

    void write(size_t len) {
        RUN_ELEVATED({
            sync::irq_lock_guard guard(conn->lock);
            (void)conn->snd_queue.append(g_bytes, len);
        });
        (void)output(conn.ptr());
    }

    int32_t ack() {
        return input(lp.remote.ack(PEER_ISS + 1, conn->snd_nxt));
    }
};

static void expect_probe(const kept_alive& k, size_t frame) {
    ASSERT_TRUE(k.lp.link.frames_sent() > frame);
    const tcp_header* hdr = sent_tcp(k.lp.link, frame);
    EXPECT_EQ(hdr->flags, FLAG_ACK);
    EXPECT_EQ(ntohl(hdr->seq), k.conn->snd_una - 1);
    EXPECT_EQ(ntohl(hdr->ack), PEER_ISS + 1);
    EXPECT_EQ(k.lp.link.frame_len(frame), eth::HEADER_LEN + ipv4::HEADER_LEN + hdr->header_len());
}

TEST(tcp_keepalive, keepalives_are_off_unless_asked_for) {
    linked_peer lp;
    kept_alive k(lp);

    EXPECT_FALSE(k.conn->keepalive);
    EXPECT_FALSE(k.conn->keepalive_armed);
    EXPECT_EQ(k.conn->keepalive_idle_s, KEEPALIVE_IDLE_S);
    EXPECT_EQ(k.conn->keepalive_interval_s, KEEPALIVE_INTERVAL_S);
    EXPECT_EQ(k.conn->keepalive_probes, KEEPALIVE_PROBES);
}

TEST(tcp_keepalive, an_idle_connection_is_probed_after_the_idle_time_and_then_at_each_interval) {
    linked_peer lp;
    kept_alive k(lp);
    k.enable();
    ASSERT_TRUE(k.conn->keepalive_armed);
    EXPECT_EQ(k.conn->keepalive_deadline_ns, k.conn->peer_acked_ns + 10 * SECOND);

    k.expire();
    expect_probe(k, 0);
    EXPECT_EQ(k.conn->unanswered_probes, 1);
    EXPECT_EQ(k.conn->keepalive_deadline_ns, g_fake_now + 2 * SECOND);

    k.expire();
    expect_probe(k, 1);
    EXPECT_EQ(k.conn->unanswered_probes, 2);

    EXPECT_EQ(k.ack(), OK);
    EXPECT_EQ(k.conn->unanswered_probes, 0);
    k.expire();
    EXPECT_EQ(lp.link.frames_sent(), 2u);
    EXPECT_EQ(k.conn->keepalive_deadline_ns, k.conn->peer_acked_ns + 10 * SECOND);
}

TEST(tcp_keepalive, the_connection_is_reset_once_the_probes_go_unanswered) {
    linked_peer lp;
    kept_alive k(lp);
    k.enable();

    for (int i = 0; i < 3; i++) {
        k.expire();
    }
    ASSERT_EQ(lp.link.frames_sent(), 3u);
    EXPECT_EQ(k.conn->state, tcp_state::established);

    k.expire();
    ASSERT_EQ(lp.link.frames_sent(), 4u);
    EXPECT_EQ(sent_tcp(lp.link, 3)->flags, FLAG_RST | FLAG_ACK);
    EXPECT_EQ(k.conn->state, tcp_state::closed);
    EXPECT_EQ(k.conn->pending_error, resource::ERR_TIMEDOUT);
    EXPECT_FALSE(lookup(tuple{lp.remote.host, lp.remote.addr, lp.remote.host_port, lp.remote.port}));
}

// The clock jump also fires the RTO of the data in flight: one retransmission, no probe
TEST(tcp_keepalive, data_in_flight_defers_the_probe) {
    linked_peer lp;
    kept_alive k(lp);
    k.enable();
    k.write(100);
    lp.link.clear_frames();

    k.expire();
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(ntohl(sent_tcp(lp.link, 0)->seq), k.conn->snd_una);
    EXPECT_EQ(k.conn->unanswered_probes, 0);
    EXPECT_EQ(k.conn->keepalive_deadline_ns, g_fake_now + 10 * SECOND);
}

TEST(tcp_keepalive, traffic_from_the_peer_restarts_the_idle_time) {
    linked_peer lp;
    kept_alive k(lp);
    k.enable();

    g_fake_now += 6 * SECOND;
    EXPECT_EQ(k.ack(), OK);
    k.expire();
    EXPECT_EQ(lp.link.frames_sent(), 0u);
    EXPECT_EQ(k.conn->keepalive_deadline_ns, k.conn->peer_acked_ns + 10 * SECOND);

    k.expire();
    expect_probe(k, 0);
}

TEST(tcp_keepalive, turning_keepalives_off_stops_the_probes) {
    linked_peer lp;
    kept_alive k(lp);
    k.enable();
    RUN_ELEVATED({
        sync::irq_lock_guard guard(k.conn->lock);
        k.conn->keepalive = false;
    });

    k.expire();
    EXPECT_EQ(lp.link.frames_sent(), 0u);
    EXPECT_FALSE(k.conn->keepalive_armed);
}
