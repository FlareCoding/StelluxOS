#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "harness.h"
#include "net/tcp/conn.h"
#include "net/tcp/listen.h"
#include "net/tcp/timewait.h"
#include "resource/resource.h"
#include "clock/clock.h"
#include "timer/timer.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(tcp_close);

using namespace net;
using namespace net::tcp;

constexpr uint32_t PEER_ISS = 9000;

static uint64_t g_fake_now;

static uint64_t fake_clock() {
    return g_fake_now;
}

static void advance_and_fire(uint64_t ns) {
    g_fake_now += ns;
    RUN_ELEVATED(timer::__dbg_test_fire_expired(g_fake_now));
}

static tuple key_of(const peer& remote) {
    return tuple{remote.host, remote.addr, remote.host_port, remote.port};
}

// A connection this host opened and established without options, so rcv_nxt
// is PEER_ISS + 1 and the FIN this host sends takes sequence iss + 1
struct closable {
    rc::strong_ref<tcp_conn> conn;
    linked_peer&             lp;

    explicit closable(linked_peer& link) : lp(link) {
        g_fake_now = clock::now_ns();
        __dbg_test_set_clock(fake_clock);

        open_active(key_of(lp.remote), &lp.link, &conn);
        input(lp.remote.segment(FLAG_SYN | FLAG_ACK, PEER_ISS, conn->iss + 1));
        lp.link.clear_frames();
    }

    // Whatever the tuple still holds leaves before the link on the stack does
    ~closable() {
        abort_connection(conn.ptr());
        if (timewait()) {
            advance_and_fire(TIMEWAIT_LEN_NS);
        }

        __dbg_test_set_clock(nullptr);
    }

    uint32_t rcv_nxt() const { return PEER_ISS + 1; }
    uint32_t fin_seq() const { return conn->iss + 1; }

    rc::strong_ref<tcp_timewait> timewait() const {
        rc::strong_ref<record> rec = lookup(key_of(lp.remote));
        if (!rec || rec->kind != record_kind::timewait) {
            return {};
        }

        rec->add_ref();
        return rc::strong_ref<tcp_timewait>::adopt(static_cast<tcp_timewait*>(rec.ptr()));
    }

    // Our close, then the peer's FIN that also acknowledges it
    void close_both_sides() {
        close_connection(conn.ptr());
        input(lp.remote.segment(FLAG_FIN | FLAG_ACK, rcv_nxt(), fin_seq() + 1));
        lp.link.clear_frames();
    }
};

static void expect_segment(const stub_interface& link, size_t frame, uint8_t flags, uint32_t seq, uint32_t ack) {
    ASSERT_TRUE(link.frames_sent() > frame);
    const tcp_header* hdr = sent_tcp(link, frame);
    EXPECT_EQ(hdr->flags, flags);
    EXPECT_EQ(ntohl(hdr->seq), seq);
    EXPECT_EQ(ntohl(hdr->ack), ack);
}

TEST(tcp_close, close_sends_a_fin_and_enters_fin_wait_1) {
    linked_peer lp;
    closable c(lp);

    close_connection(c.conn.ptr());

    EXPECT_EQ(lp.link.frames_sent(), 1u);
    expect_segment(lp.link, 0, FLAG_FIN | FLAG_ACK, c.fin_seq(), c.rcv_nxt());
    EXPECT_EQ(c.conn->state, tcp_state::fin_wait_1);
    EXPECT_TRUE(c.conn->fin_sent);
    EXPECT_TRUE(c.conn->orphaned);
    EXPECT_EQ(c.conn->snd_nxt, c.fin_seq() + 1);
    EXPECT_EQ(c.conn->send_timer_kind, timer_kind::rto);
}

TEST(tcp_close, the_ack_of_our_fin_moves_to_fin_wait_2_and_stops_the_timer) {
    linked_peer lp;
    closable c(lp);

    close_connection(c.conn.ptr());
    EXPECT_EQ(input(lp.remote.ack(c.rcv_nxt(), c.fin_seq() + 1)), OK);

    EXPECT_EQ(c.conn->state, tcp_state::fin_wait_2);
    EXPECT_EQ(c.conn->send_timer_kind, timer_kind::none);
    EXPECT_EQ(lp.link.frames_sent(), 1u);
}

TEST(tcp_close, the_peers_fin_in_fin_wait_2_is_acked_and_enters_time_wait) {
    linked_peer lp;
    closable c(lp);

    close_connection(c.conn.ptr());
    EXPECT_EQ(input(lp.remote.ack(c.rcv_nxt(), c.fin_seq() + 1)), OK);
    lp.link.clear_frames();

    EXPECT_EQ(input(lp.remote.segment(FLAG_FIN | FLAG_ACK, c.rcv_nxt(), c.fin_seq() + 1)), OK);

    EXPECT_EQ(lp.link.frames_sent(), 1u);
    expect_segment(lp.link, 0, FLAG_ACK, c.fin_seq() + 1, c.rcv_nxt() + 1);
    EXPECT_EQ(c.conn->state, tcp_state::closed);
    EXPECT_EQ(record_count(record_kind::connection), 0u);

    rc::strong_ref<tcp_timewait> tw = c.timewait();
    ASSERT_TRUE(tw);
    EXPECT_EQ(tw->snd_nxt, c.fin_seq() + 1);
    EXPECT_EQ(tw->rcv_nxt, c.rcv_nxt() + 1);
    EXPECT_TRUE(tw->timer_armed);
    EXPECT_EQ(record_count(record_kind::timewait), 1u);
}

TEST(tcp_close, a_fin_that_also_acks_our_fin_enters_time_wait_at_once) {
    linked_peer lp;
    closable c(lp);

    close_connection(c.conn.ptr());
    lp.link.clear_frames();
    EXPECT_EQ(input(lp.remote.segment(FLAG_FIN | FLAG_ACK, c.rcv_nxt(), c.fin_seq() + 1)), OK);

    expect_segment(lp.link, 0, FLAG_ACK, c.fin_seq() + 1, c.rcv_nxt() + 1);
    EXPECT_EQ(c.conn->state, tcp_state::closed);
    EXPECT_TRUE(c.timewait());
}

TEST(tcp_close, a_simultaneous_close_passes_through_closing) {
    linked_peer lp;
    closable c(lp);

    close_connection(c.conn.ptr());
    lp.link.clear_frames();
    EXPECT_EQ(input(lp.remote.segment(FLAG_FIN | FLAG_ACK, c.rcv_nxt(), c.fin_seq())), OK);

    expect_segment(lp.link, 0, FLAG_ACK, c.fin_seq() + 1, c.rcv_nxt() + 1);
    EXPECT_EQ(c.conn->state, tcp_state::closing);
    EXPECT_FALSE(c.timewait());

    EXPECT_EQ(input(lp.remote.ack(c.rcv_nxt() + 1, c.fin_seq() + 1)), OK);
    EXPECT_EQ(c.conn->state, tcp_state::closed);
    EXPECT_TRUE(c.timewait());
}

TEST(tcp_close, the_peer_closing_first_ends_in_closed_without_time_wait) {
    linked_peer lp;
    closable c(lp);

    EXPECT_EQ(input(lp.remote.segment(FLAG_FIN | FLAG_ACK, c.rcv_nxt(), c.fin_seq())), OK);
    expect_segment(lp.link, 0, FLAG_ACK, c.fin_seq(), c.rcv_nxt() + 1);
    EXPECT_EQ(c.conn->state, tcp_state::close_wait);
    EXPECT_TRUE(c.conn->fin_rcvd);
    EXPECT_EQ(c.conn->rcv_nxt, c.rcv_nxt() + 1);

    close_connection(c.conn.ptr());
    expect_segment(lp.link, 1, FLAG_FIN | FLAG_ACK, c.fin_seq(), c.rcv_nxt() + 1);
    EXPECT_EQ(c.conn->state, tcp_state::last_ack);

    EXPECT_EQ(input(lp.remote.ack(c.rcv_nxt() + 1, c.fin_seq() + 1)), OK);
    EXPECT_EQ(c.conn->state, tcp_state::closed);
    EXPECT_EQ(c.conn->pending_error, resource::OK);
    EXPECT_EQ(lp.link.frames_sent(), 2u);
    EXPECT_FALSE(lookup(key_of(lp.remote)));
    EXPECT_EQ(record_count(record_kind::timewait), 0u);
}

TEST(tcp_close, a_fin_behind_payload_waits_for_the_data_path) {
    linked_peer lp;
    closable c(lp);

    uint8_t byte = 'x';
    EXPECT_EQ(input(lp.remote.segment(FLAG_FIN | FLAG_ACK, c.rcv_nxt(), c.fin_seq(), {}, &byte, 1)), OK);

    EXPECT_EQ(c.conn->state, tcp_state::established);
    EXPECT_FALSE(c.conn->fin_rcvd);
    EXPECT_EQ(c.conn->rcv_nxt, c.rcv_nxt());
    EXPECT_EQ(lp.link.frames_sent(), 0u);
}

TEST(tcp_close, the_fin_is_retransmitted_with_backoff_then_given_up) {
    linked_peer lp;
    closable c(lp);

    close_connection(c.conn.ptr());
    lp.link.clear_frames();

    for (size_t i = 0; i < ORPHAN_RETRIES; i++) {
        advance_and_fire(TIMEOUT_MAX_NS);
        EXPECT_EQ(lp.link.frames_sent(), i + 1);
        expect_segment(lp.link, i, FLAG_FIN | FLAG_ACK, c.fin_seq(), c.rcv_nxt());
        EXPECT_EQ(c.conn->state, tcp_state::fin_wait_1);
    }

    advance_and_fire(TIMEOUT_MAX_NS);
    EXPECT_EQ(lp.link.frames_sent(), static_cast<size_t>(ORPHAN_RETRIES));
    EXPECT_EQ(c.conn->state, tcp_state::closed);
    EXPECT_EQ(c.conn->pending_error, resource::ERR_TIMEDOUT);
    EXPECT_FALSE(lookup(key_of(lp.remote)));
}

TEST(tcp_close, time_wait_acks_a_retransmitted_fin_and_restarts_its_wait) {
    linked_peer lp;
    closable c(lp);
    c.close_both_sides();

    rc::strong_ref<tcp_timewait> tw = c.timewait();
    ASSERT_TRUE(tw);
    uint64_t first_deadline = tw->timer_deadline_ns;

    g_fake_now += 10000000000ULL;
    EXPECT_EQ(input(lp.remote.segment(FLAG_FIN | FLAG_ACK, c.rcv_nxt(), c.fin_seq() + 1)), OK);

    expect_segment(lp.link, 0, FLAG_ACK, c.fin_seq() + 1, c.rcv_nxt() + 1);
    EXPECT_EQ(tw->timer_deadline_ns, first_deadline + 10000000000ULL);
    EXPECT_TRUE(c.timewait());
}

TEST(tcp_close, time_wait_ignores_a_reset_and_expires_after_two_msl) {
    linked_peer lp;
    closable c(lp);
    c.close_both_sides();

    EXPECT_EQ(input(lp.remote.rst(c.rcv_nxt() + 1)), OK);
    EXPECT_TRUE(c.timewait());
    EXPECT_EQ(lp.link.frames_sent(), 0u);

    advance_and_fire(TIMEWAIT_LEN_NS - 1);
    EXPECT_TRUE(c.timewait());

    advance_and_fire(1);
    EXPECT_FALSE(c.timewait());
    EXPECT_EQ(record_count(record_kind::timewait), 0u);
}

TEST(tcp_close, time_wait_drops_a_stray_ack_and_an_old_syn) {
    linked_peer lp;
    closable c(lp);
    c.close_both_sides();

    EXPECT_EQ(input(lp.remote.ack(c.rcv_nxt() + 1, c.fin_seq() + 1)), OK);
    EXPECT_EQ(input(lp.remote.syn(c.rcv_nxt())), OK);

    EXPECT_EQ(lp.link.frames_sent(), 0u);
    EXPECT_TRUE(c.timewait());
}

TEST(tcp_close, time_wait_reopens_for_a_newer_syn_when_a_listener_waits) {
    linked_peer lp;
    tcp_listener* listener = alloc_listener(endpoint{ipv4::UNSPECIFIED_ADDR, 5000, nullptr, false});
    listener->backlog = 8;
    ASSERT_EQ(listener_insert(listener), OK);

    {
        closable c(lp);
        c.close_both_sides();

        EXPECT_EQ(input(lp.remote.syn(c.rcv_nxt() + 1000)), OK);

        EXPECT_FALSE(c.timewait());
        EXPECT_EQ(record_count(record_kind::timewait), 0u);
        expect_segment(lp.link, 0, FLAG_SYN | FLAG_ACK, ntohl(sent_tcp(lp.link, 0)->seq), c.rcv_nxt() + 1001);

        rc::strong_ref<record> rec = lookup(key_of(lp.remote));
        ASSERT_TRUE(rec);
        EXPECT_EQ(rec->kind, record_kind::request);
    }

    listener_close(listener);
    if (listener->release()) {
        tcp_listener::ref_destroy(listener);
    }
}

TEST(tcp_close, time_wait_resets_a_newer_syn_without_a_listener) {
    linked_peer lp;
    closable c(lp);
    c.close_both_sides();

    EXPECT_EQ(input(lp.remote.syn(c.rcv_nxt() + 1000)), OK);

    EXPECT_FALSE(c.timewait());
    expect_segment(lp.link, 0, FLAG_RST | FLAG_ACK, 0, c.rcv_nxt() + 1001);
    EXPECT_FALSE(lookup(key_of(lp.remote)));
}
