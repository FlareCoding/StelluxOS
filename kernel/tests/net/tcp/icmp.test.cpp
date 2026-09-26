#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "harness.h"
#include "net/tcp/conn.h"
#include "net/tcp/output.h"
#include "net/icmp.h"
#include "net/net.h"
#include "resource/resource.h"
#include "clock/clock.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(tcp_icmp);

using namespace net;
using namespace net::tcp;

constexpr uint32_t PEER_ISS = 8000;
constexpr uint16_t PEER_MSS = 1000;

static uint8_t g_bytes[PEER_MSS];

static tuple key_of(const peer& remote) {
    return tuple{remote.host, remote.addr, remote.host_port, remote.port};
}

// A connection this host is opening, its SYN outstanding, that ICMP errors will reach
struct informed {
    rc::strong_ref<tcp_conn> conn;
    linked_peer&             lp;

    explicit informed(linked_peer& link) : lp(link) {
        open_active(key_of(lp.remote), &lp.link, &conn);
        lp.link.clear_frames();
    }

    ~informed() {
        abort_connection(conn.ptr());
    }

    void establish() {
        tcp_options opts;
        opts.mss = PEER_MSS;
        input(lp.remote.segment(FLAG_SYN | FLAG_ACK, PEER_ISS, conn->iss + 1, opts));
        lp.link.clear_frames();
    }

    void write(size_t len) {
        RUN_ELEVATED({
            sync::irq_lock_guard guard(conn->lock);
            (void)conn->snd_queue.append(g_bytes, len);
        });
        (void)output(conn.ptr());
    }

    int32_t error(uint8_t type, uint8_t code, uint32_t offending_seq) {
        return icmp::input(lp.remote.icmp_error(type, code, offending_seq));
    }
};

TEST(tcp_icmp, an_unreachable_host_during_the_handshake_fails_the_connect_with_that_error) {
    linked_peer lp;
    informed c(lp);
    ASSERT_EQ(c.conn->state, tcp_state::syn_sent);

    EXPECT_EQ(c.error(icmp::TYPE_DEST_UNREACHABLE, icmp::CODE_HOST_UNREACHABLE, c.conn->iss), OK);
    EXPECT_EQ(c.conn->state, tcp_state::closed);
    EXPECT_EQ(c.conn->pending_error, resource::ERR_HOSTUNREACH);
    EXPECT_FALSE(lookup(key_of(lp.remote)));
}

TEST(tcp_icmp, a_refused_port_during_the_handshake_fails_the_connect_as_refused) {
    linked_peer lp;
    informed c(lp);

    EXPECT_EQ(c.error(icmp::TYPE_DEST_UNREACHABLE, icmp::CODE_PORT_UNREACHABLE, c.conn->iss), OK);
    EXPECT_EQ(c.conn->state, tcp_state::closed);
    EXPECT_EQ(c.conn->pending_error, resource::ERR_CONNREFUSED);
}

TEST(tcp_icmp, an_error_about_a_sequence_not_outstanding_is_ignored) {
    linked_peer lp;
    informed c(lp);

    EXPECT_EQ(c.error(icmp::TYPE_DEST_UNREACHABLE, icmp::CODE_HOST_UNREACHABLE, c.conn->iss + 5), OK);
    EXPECT_EQ(c.error(icmp::TYPE_DEST_UNREACHABLE, icmp::CODE_HOST_UNREACHABLE, c.conn->iss - 1), OK);
    EXPECT_EQ(c.conn->state, tcp_state::syn_sent);
    EXPECT_TRUE(lookup(key_of(lp.remote)));
}

TEST(tcp_icmp, an_established_connection_records_the_error_and_carries_on) {
    linked_peer lp;
    informed c(lp);
    c.establish();
    c.write(100);
    uint32_t outstanding = c.conn->snd_una;

    EXPECT_EQ(c.error(icmp::TYPE_DEST_UNREACHABLE, icmp::CODE_HOST_UNREACHABLE, outstanding), OK);
    EXPECT_EQ(c.conn->state, tcp_state::established);
    EXPECT_EQ(c.conn->soft_error, resource::ERR_HOSTUNREACH);

    EXPECT_EQ(c.error(icmp::TYPE_DEST_UNREACHABLE, icmp::CODE_PORT_UNREACHABLE, outstanding), OK);
    EXPECT_EQ(c.conn->state, tcp_state::established);
    EXPECT_EQ(c.conn->soft_error, resource::ERR_CONNREFUSED);

    EXPECT_EQ(c.error(icmp::TYPE_TIME_EXCEEDED, icmp::CODE_TTL_EXCEEDED, outstanding), OK);
    EXPECT_EQ(c.conn->soft_error, resource::ERR_HOSTUNREACH);
    EXPECT_EQ(c.error(icmp::TYPE_PARAMETER_PROBLEM, 0, outstanding), OK);
    EXPECT_EQ(c.conn->soft_error, resource::ERR_PROTO);

    EXPECT_EQ(input(lp.remote.ack(PEER_ISS + 1, c.conn->snd_nxt)), OK);
    EXPECT_EQ(c.conn->soft_error, OK);
}

TEST(tcp_icmp, an_error_with_nothing_outstanding_or_about_another_port_is_ignored) {
    linked_peer lp;
    informed c(lp);
    c.establish();

    EXPECT_EQ(c.error(icmp::TYPE_DEST_UNREACHABLE, icmp::CODE_HOST_UNREACHABLE, c.conn->snd_una), OK);
    EXPECT_EQ(c.conn->soft_error, OK);

    c.write(100);
    peer other = lp.remote;
    other.port++;
    EXPECT_EQ(icmp::input(other.icmp_error(icmp::TYPE_DEST_UNREACHABLE, icmp::CODE_HOST_UNREACHABLE, c.conn->snd_una)), OK);
    EXPECT_EQ(c.conn->soft_error, OK);
    EXPECT_EQ(c.conn->state, tcp_state::established);
}

TEST(tcp_icmp, an_error_that_is_not_about_reachability_is_left_alone) {
    linked_peer lp;
    informed c(lp);
    c.establish();
    c.write(100);

    EXPECT_EQ(c.error(icmp::TYPE_DEST_UNREACHABLE, icmp::CODE_FRAGMENTATION_NEEDED, c.conn->snd_una), OK);
    EXPECT_EQ(c.conn->soft_error, OK);
    EXPECT_EQ(c.conn->state, tcp_state::established);
}
