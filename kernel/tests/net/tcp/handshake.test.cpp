#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "harness.h"
#include "net/tcp/conn.h"
#include "net/tcp/listen.h"
#include "net/tcp/seq.h"
#include "resource/resource.h"
#include "clock/clock.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(tcp_handshake);

using namespace net;
using namespace net::tcp;

constexpr uint64_t SECOND_NS = 1000000000ULL;

static uint64_t g_fake_now;

static uint64_t fake_clock() {
    return g_fake_now;
}

// A listener on the harness port whose requests are cleared with it
struct listening {
    tcp_listener* listener;

    explicit listening(uint16_t backlog) {
        listener = alloc_listener(endpoint{ipv4::UNSPECIFIED_ADDR, 5000, nullptr, false});
        listener->backlog = backlog;
        listener_insert(listener);
        g_fake_now = clock::now_ns();
        __dbg_test_set_clock(fake_clock);
    }

    ~listening() {
        listener_close(listener);
        if (listener->release()) {
            tcp_listener::ref_destroy(listener);
        }

        __dbg_test_set_clock(nullptr);
    }

    uint16_t request_count() const {
        uint16_t count = 0;
        RUN_ELEVATED({
            sync::irq_lock_guard guard(listener->lock);
            count = listener->request_count;
        });

        return count;
    }
};

static tuple key_of(const peer& remote) {
    return tuple{remote.host, remote.addr, remote.host_port, remote.port};
}

static void advance_and_fire(uint64_t ns) {
    g_fake_now += ns;
    RUN_ELEVATED(timer::__dbg_test_fire_expired(g_fake_now));
}

static tcp_options synack_options(const linked_peer& lp, size_t frame) {
    tcp_options opts;
    parse_options(sent_tcp(lp.link, frame), &opts);
    return opts;
}

static tcp_options full_syn_options() {
    tcp_options opts;
    opts.mss = 1460;
    opts.sack_permitted = true;
    opts.has_timestamps = true;
    opts.ts_val = 777;
    opts.window_scale = 7;
    return opts;
}

TEST(tcp_handshake, syn_is_answered_with_a_synack_carrying_the_negotiated_options) {
    linked_peer lp;
    listening on(8);

    EXPECT_EQ(input(lp.remote.segment(FLAG_SYN, 1000, 0, full_syn_options())), OK);
    ASSERT_EQ(lp.link.frames_sent(), 1u);

    const tcp_header* hdr = sent_tcp(lp.link, 0);
    EXPECT_EQ(hdr->flags, FLAG_SYN | FLAG_ACK);
    EXPECT_EQ(ntohl(hdr->ack), 1001u);
    EXPECT_EQ(ntohs(hdr->window), RCV_WND_INITIAL);
    EXPECT_EQ(ntohs(hdr->src_port), 5000);
    EXPECT_EQ(ntohs(hdr->dst_port), 40000);

    tcp_options opts = synack_options(lp, 0);
    EXPECT_EQ(opts.mss, 1460);
    EXPECT_TRUE(opts.sack_permitted);
    EXPECT_TRUE(opts.has_timestamps);
    EXPECT_EQ(opts.ts_ecr, 777u);
    EXPECT_EQ(opts.window_scale, receive_window_scale());
    EXPECT_EQ(opts.window_scale, 5);

    rc::strong_ref<record> rec = lookup(key_of(lp.remote));
    ASSERT_TRUE(rec);
    EXPECT_EQ(rec->kind, record_kind::request);
    const tcp_request* request = static_cast<const tcp_request*>(rec.ptr());
    EXPECT_EQ(request->irs, 1000u);
    EXPECT_EQ(request->iss, ntohl(hdr->seq));
    EXPECT_EQ(request->peer_mss, 1460);
    EXPECT_EQ(request->snd_wscale, 7);
    EXPECT_TRUE(request->timer_armed);
    EXPECT_EQ(on.request_count(), 1);
}

TEST(tcp_handshake, syn_without_options_gets_only_an_mss) {
    linked_peer lp;
    listening on(8);

    EXPECT_EQ(input(lp.remote.syn(1000)), OK);
    ASSERT_EQ(lp.link.frames_sent(), 1u);

    tcp_options opts = synack_options(lp, 0);
    EXPECT_EQ(opts.mss, 1460);
    EXPECT_FALSE(opts.sack_permitted);
    EXPECT_FALSE(opts.has_timestamps);
    EXPECT_EQ(opts.window_scale, WINDOW_SCALE_NONE);

    rc::strong_ref<record> rec = lookup(key_of(lp.remote));
    ASSERT_TRUE(rec);
    const tcp_request* request = static_cast<const tcp_request*>(rec.ptr());
    EXPECT_EQ(request->peer_mss, DEFAULT_MSS);
    EXPECT_FALSE(request->wscale_ok);
    EXPECT_EQ(request->snd_wscale, 0);
    EXPECT_EQ(request->rcv_wscale, 0);
}

TEST(tcp_handshake, retransmitted_syn_is_answered_again_with_the_same_numbers) {
    linked_peer lp;
    listening on(8);

    EXPECT_EQ(input(lp.remote.syn(1000)), OK);
    EXPECT_EQ(input(lp.remote.syn(1000)), OK);
    ASSERT_EQ(lp.link.frames_sent(), 2u);

    EXPECT_EQ(sent_tcp(lp.link, 1)->flags, FLAG_SYN | FLAG_ACK);
    EXPECT_EQ(ntohl(sent_tcp(lp.link, 1)->seq), ntohl(sent_tcp(lp.link, 0)->seq));
    EXPECT_EQ(ntohl(sent_tcp(lp.link, 1)->ack), 1001u);
    EXPECT_EQ(on.request_count(), 1);
    EXPECT_EQ(record_count(record_kind::request), 1u);
}

TEST(tcp_handshake, full_backlog_drops_further_syns_silently) {
    linked_peer lp;
    listening on(1);
    peer second = lp.remote;
    second.port = 40001;

    EXPECT_EQ(input(lp.remote.syn(1000)), OK);
    EXPECT_EQ(input(second.syn(2000)), OK);

    EXPECT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(on.request_count(), 1);
    EXPECT_FALSE(lookup(key_of(second)));
}

TEST(tcp_handshake, synack_is_retransmitted_with_backoff_until_the_request_is_given_up) {
    linked_peer lp;
    listening on(8);
    EXPECT_EQ(input(lp.remote.syn(1000)), OK);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    uint32_t iss = ntohl(sent_tcp(lp.link, 0)->seq);

    // Nothing fires before the deadline
    advance_and_fire(SECOND_NS / 2);
    EXPECT_EQ(lp.link.frames_sent(), 1u);

    uint64_t backoff = TIMEOUT_INIT_NS;
    for (size_t attempt = 1; attempt <= SYNACK_RETRIES; attempt++) {
        advance_and_fire(backoff);
        ASSERT_EQ(lp.link.frames_sent(), attempt + 1);
        EXPECT_EQ(sent_tcp(lp.link, attempt)->flags, FLAG_SYN | FLAG_ACK);
        EXPECT_EQ(ntohl(sent_tcp(lp.link, attempt)->seq), iss);
        backoff *= 2;
    }

    EXPECT_TRUE(lookup(key_of(lp.remote)));
    advance_and_fire(backoff);
    EXPECT_EQ(lp.link.frames_sent(), static_cast<size_t>(SYNACK_RETRIES) + 1);
    EXPECT_FALSE(lookup(key_of(lp.remote)));
    EXPECT_EQ(on.request_count(), 0);
}

TEST(tcp_handshake, closing_the_listener_retires_its_requests) {
    linked_peer lp;
    tcp_listener* listener = alloc_listener(endpoint{ipv4::UNSPECIFIED_ADDR, 5000, nullptr, false});
    listener->backlog = 8;
    ASSERT_EQ(listener_insert(listener), OK);
    g_fake_now = clock::now_ns();
    __dbg_test_set_clock(fake_clock);

    EXPECT_EQ(input(lp.remote.syn(1000)), OK);
    ASSERT_TRUE(lookup(key_of(lp.remote)));

    listener_close(listener);
    EXPECT_FALSE(lookup(key_of(lp.remote)));
    EXPECT_EQ(record_count(record_kind::request), 0u);

    // A SYN that looked the listener up before it closed must not revive it
    packet* late = lp.remote.syn(3000);
    const tcp_header* late_hdr = reinterpret_cast<const tcp_header*>(late->data());
    EXPECT_EQ(listen_input(listener, late, late_hdr, tcp_options{}), OK);
    EXPECT_FALSE(lookup(key_of(lp.remote)));
    EXPECT_EQ(lp.link.frames_sent(), 1u);

    advance_and_fire(TIMEOUT_INIT_NS * 2);
    EXPECT_EQ(lp.link.frames_sent(), 1u);

    if (listener->release()) {
        tcp_listener::ref_destroy(listener);
    }

    __dbg_test_set_clock(nullptr);
}

static uint32_t sent_seq(const linked_peer& lp, size_t frame) {
    return ntohl(sent_tcp(lp.link, frame)->seq);
}

TEST(tcp_handshake, completing_ack_promotes_the_request_to_an_established_connection) {
    linked_peer lp;
    listening on(8);
    EXPECT_EQ(input(lp.remote.segment(FLAG_SYN, 1000, 0, full_syn_options())), OK);
    uint32_t iss = sent_seq(lp, 0);

    EXPECT_EQ(input(lp.remote.ack(1001, iss + 1)), OK);
    EXPECT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(record_count(record_kind::request), 0u);
    EXPECT_EQ(on.request_count(), 0);

    rc::strong_ref<record> rec = lookup(key_of(lp.remote));
    ASSERT_TRUE(rec);
    ASSERT_EQ(rec->kind, record_kind::connection);
    const tcp_conn* conn = static_cast<const tcp_conn*>(rec.ptr());
    EXPECT_EQ(conn->state, tcp_state::established);
    EXPECT_TRUE(conn->passive);
    EXPECT_EQ(conn->iss, iss);
    EXPECT_EQ(conn->irs, 1000u);
    EXPECT_EQ(conn->snd_una, iss + 1);
    EXPECT_EQ(conn->snd_nxt, iss + 1);
    EXPECT_EQ(conn->rcv_nxt, 1001u);
    EXPECT_EQ(conn->snd_wnd, 65535u << 7);
    EXPECT_EQ(conn->snd_wscale, 7);
    EXPECT_EQ(conn->rcv_wscale, 5);
    EXPECT_EQ(conn->snd_mss, 1460);
    EXPECT_EQ(conn->rcv_wnd, RCV_WND_INITIAL);
    EXPECT_TRUE(conn->sack_ok);
    EXPECT_TRUE(conn->ts_ok);
    EXPECT_EQ(conn->ts_recent, 777u);

    // A segment for the connection is consumed, never reset
    EXPECT_EQ(input(lp.remote.ack(1001, iss + 1)), OK);
    EXPECT_EQ(lp.link.frames_sent(), 1u);

    rc::strong_ref<tcp_conn> accepted = pop_accepted(on.listener);
    ASSERT_TRUE(accepted);
    EXPECT_EQ(static_cast<const record*>(accepted.ptr()), rec.ptr());
    EXPECT_FALSE(pop_accepted(on.listener));
    abort_connection(accepted.ptr());
}

TEST(tcp_handshake, plain_syn_connection_takes_the_default_mss) {
    linked_peer lp;
    listening on(8);
    EXPECT_EQ(input(lp.remote.syn(1000)), OK);
    EXPECT_EQ(input(lp.remote.ack(1001, sent_seq(lp, 0) + 1)), OK);

    rc::strong_ref<tcp_conn> conn = pop_accepted(on.listener);
    ASSERT_TRUE(conn);
    EXPECT_EQ(conn->snd_mss, DEFAULT_MSS);
    EXPECT_EQ(conn->snd_wnd, 65535u);
    EXPECT_FALSE(conn->wscale_ok);
    abort_connection(conn.ptr());
}

TEST(tcp_handshake, ack_with_the_wrong_acknowledgment_is_reset_and_the_request_stays) {
    linked_peer lp;
    listening on(8);
    EXPECT_EQ(input(lp.remote.syn(1000)), OK);
    uint32_t iss = sent_seq(lp, 0);

    EXPECT_EQ(input(lp.remote.ack(1001, iss + 5)), OK);
    ASSERT_EQ(lp.link.frames_sent(), 2u);
    EXPECT_EQ(sent_tcp(lp.link, 1)->flags, FLAG_RST);
    EXPECT_EQ(sent_seq(lp, 1), iss + 5);

    rc::strong_ref<record> rec = lookup(key_of(lp.remote));
    ASSERT_TRUE(rec);
    EXPECT_EQ(rec->kind, record_kind::request);
    EXPECT_EQ(on.request_count(), 1);
}

TEST(tcp_handshake, ack_with_the_wrong_sequence_gets_the_handshake_ack_again) {
    linked_peer lp;
    listening on(8);
    EXPECT_EQ(input(lp.remote.syn(1000)), OK);
    uint32_t iss = sent_seq(lp, 0);

    EXPECT_EQ(input(lp.remote.ack(1500, iss + 1)), OK);
    ASSERT_EQ(lp.link.frames_sent(), 2u);
    EXPECT_EQ(sent_tcp(lp.link, 1)->flags, FLAG_ACK);
    EXPECT_EQ(sent_seq(lp, 1), iss + 1);
    EXPECT_EQ(ntohl(sent_tcp(lp.link, 1)->ack), 1001u);
    EXPECT_EQ(lookup(key_of(lp.remote))->kind, record_kind::request);
}

TEST(tcp_handshake, paws_rejects_a_completing_ack_with_an_older_timestamp) {
    linked_peer lp;
    listening on(8);
    EXPECT_EQ(input(lp.remote.segment(FLAG_SYN, 1000, 0, full_syn_options())), OK);
    uint32_t iss = sent_seq(lp, 0);

    tcp_options older;
    older.has_timestamps = true;
    older.ts_val = 700;
    EXPECT_EQ(input(lp.remote.segment(FLAG_ACK, 1001, iss + 1, older)), OK);
    EXPECT_EQ(lp.link.frames_sent(), 2u);
    EXPECT_EQ(sent_tcp(lp.link, 1)->flags, FLAG_ACK);
    EXPECT_EQ(lookup(key_of(lp.remote))->kind, record_kind::request);

    tcp_options newer = older;
    newer.ts_val = 800;
    EXPECT_EQ(input(lp.remote.segment(FLAG_ACK, 1001, iss + 1, newer)), OK);
    rc::strong_ref<tcp_conn> conn = pop_accepted(on.listener);
    ASSERT_TRUE(conn);
    EXPECT_EQ(conn->ts_recent, 800u);
    abort_connection(conn.ptr());
}

TEST(tcp_handshake, accept_hands_out_connections_in_arrival_order) {
    linked_peer lp;
    listening on(8);
    peer second = lp.remote;
    second.port = 40001;

    EXPECT_EQ(input(lp.remote.syn(1000)), OK);
    EXPECT_EQ(input(second.syn(2000)), OK);
    EXPECT_EQ(input(lp.remote.ack(1001, sent_seq(lp, 0) + 1)), OK);
    EXPECT_EQ(input(second.ack(2001, sent_seq(lp, 1) + 1)), OK);

    rc::strong_ref<tcp_conn> first_conn = pop_accepted(on.listener);
    rc::strong_ref<tcp_conn> second_conn = pop_accepted(on.listener);
    ASSERT_TRUE(first_conn);
    ASSERT_TRUE(second_conn);
    EXPECT_EQ(first_conn->key.remote_port, 40000);
    EXPECT_EQ(second_conn->key.remote_port, 40001);
    EXPECT_FALSE(pop_accepted(on.listener));

    abort_connection(first_conn.ptr());
    abort_connection(second_conn.ptr());
}

TEST(tcp_handshake, closing_the_listener_resets_connections_nobody_accepted) {
    linked_peer lp;
    listening on(8);
    EXPECT_EQ(input(lp.remote.syn(1000)), OK);
    uint32_t iss = sent_seq(lp, 0);
    EXPECT_EQ(input(lp.remote.ack(1001, iss + 1)), OK);
    ASSERT_TRUE(lookup(key_of(lp.remote)));

    listener_close(on.listener);
    ASSERT_EQ(lp.link.frames_sent(), 2u);
    EXPECT_EQ(sent_tcp(lp.link, 1)->flags, FLAG_RST | FLAG_ACK);
    EXPECT_EQ(sent_seq(lp, 1), iss + 1);
    EXPECT_EQ(ntohl(sent_tcp(lp.link, 1)->ack), 1001u);
    EXPECT_FALSE(lookup(key_of(lp.remote)));
    EXPECT_EQ(record_count(record_kind::connection), 0u);
}

TEST(tcp_handshake, reset_at_the_expected_sequence_returns_the_request_to_listen) {
    linked_peer lp;
    listening on(8);
    EXPECT_EQ(input(lp.remote.syn(1000)), OK);
    ASSERT_TRUE(lookup(key_of(lp.remote)));

    EXPECT_EQ(input(lp.remote.rst(1001)), OK);
    EXPECT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_FALSE(lookup(key_of(lp.remote)));
    EXPECT_EQ(on.request_count(), 0);

    // The slot is free again for the same peer
    EXPECT_EQ(input(lp.remote.syn(5000)), OK);
    EXPECT_EQ(lp.link.frames_sent(), 2u);
    EXPECT_EQ(sent_tcp(lp.link, 1)->flags, FLAG_SYN | FLAG_ACK);
}

TEST(tcp_handshake, reset_elsewhere_in_the_window_is_challenged_and_outside_it_ignored) {
    linked_peer lp;
    listening on(8);
    EXPECT_EQ(input(lp.remote.syn(1000)), OK);
    uint32_t iss = sent_seq(lp, 0);

    EXPECT_EQ(input(lp.remote.rst(1500)), OK);
    ASSERT_EQ(lp.link.frames_sent(), 2u);
    EXPECT_EQ(sent_tcp(lp.link, 1)->flags, FLAG_ACK);
    EXPECT_EQ(sent_seq(lp, 1), iss + 1);
    EXPECT_EQ(ntohl(sent_tcp(lp.link, 1)->ack), 1001u);

    EXPECT_EQ(input(lp.remote.rst(1001 + RCV_WND_INITIAL)), OK);
    EXPECT_EQ(lp.link.frames_sent(), 2u);
    EXPECT_EQ(lookup(key_of(lp.remote))->kind, record_kind::request);
    EXPECT_EQ(on.request_count(), 1);
}

TEST(tcp_handshake, full_accept_queue_keeps_the_request_until_accept_makes_room) {
    linked_peer lp;
    listening on(2);
    peer second = lp.remote;
    second.port = 40001;
    peer third = lp.remote;
    third.port = 40002;

    EXPECT_EQ(input(lp.remote.syn(1000)), OK);
    EXPECT_EQ(input(second.syn(2000)), OK);
    EXPECT_EQ(input(lp.remote.ack(1001, sent_seq(lp, 0) + 1)), OK);
    EXPECT_EQ(input(third.syn(3000)), OK);
    EXPECT_EQ(input(second.ack(2001, sent_seq(lp, 1) + 1)), OK);
    ASSERT_EQ(lp.link.frames_sent(), 3u);

    // Two connections wait, the third ACK finds no room and the request stays
    uint32_t third_iss = sent_seq(lp, 2);
    EXPECT_EQ(input(third.ack(3001, third_iss + 1)), OK);
    EXPECT_EQ(lp.link.frames_sent(), 3u);
    EXPECT_EQ(lookup(key_of(third))->kind, record_kind::request);
    EXPECT_EQ(on.request_count(), 1);

    rc::strong_ref<tcp_conn> first_conn = pop_accepted(on.listener);
    ASSERT_TRUE(first_conn);
    EXPECT_EQ(input(third.ack(3001, third_iss + 1)), OK);
    EXPECT_EQ(lookup(key_of(third))->kind, record_kind::connection);
    EXPECT_EQ(on.request_count(), 0);

    abort_connection(first_conn.ptr());
    while (rc::strong_ref<tcp_conn> conn = pop_accepted(on.listener)) {
        abort_connection(conn.ptr());
    }
}

TEST(tcp_handshake, completing_ack_for_a_closed_listener_resets_the_peer) {
    linked_peer lp;
    tcp_listener* listener = alloc_listener(endpoint{ipv4::UNSPECIFIED_ADDR, 5000, nullptr, false});
    listener->backlog = 8;
    ASSERT_EQ(listener_insert(listener), OK);
    g_fake_now = clock::now_ns();
    __dbg_test_set_clock(fake_clock);

    EXPECT_EQ(input(lp.remote.syn(1000)), OK);
    uint32_t iss = sent_seq(lp, 0);
    rc::strong_ref<record> request = lookup(key_of(lp.remote));
    ASSERT_TRUE(request);

    // The ACK looked its request up before the socket closed
    listener_close(listener);
    packet* ack = lp.remote.ack(1001, iss + 1);
    const tcp_header* hdr = reinterpret_cast<const tcp_header*>(ack->data());
    tcp_options opts;
    parse_options(hdr, &opts);
    EXPECT_EQ(request_input(static_cast<tcp_request*>(request.ptr()), ack, hdr, opts), OK);

    ASSERT_EQ(lp.link.frames_sent(), 2u);
    EXPECT_EQ(sent_tcp(lp.link, 1)->flags, FLAG_RST);
    EXPECT_EQ(sent_seq(lp, 1), iss + 1);
    EXPECT_FALSE(lookup(key_of(lp.remote)));
    EXPECT_EQ(record_count(record_kind::connection), 0u);

    if (listener->release()) {
        tcp_listener::ref_destroy(listener);
    }

    __dbg_test_set_clock(nullptr);
}

TEST(tcp_handshake, connection_capacity_keeps_the_request_with_its_timer_running) {
    linked_peer lp;
    listening on(8);

    // Fill the connection table with records of another peer
    static tcp_conn* filler[MAX_CONNECTIONS];
    for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
        tuple key = {lp.remote.host, {{10, 0, 2, 200}}, 6000, static_cast<uint16_t>(40000 + i)};
        filler[i] = alloc_conn(key, &lp.link);
        ASSERT_NOT_NULL(filler[i]);
        ASSERT_EQ(insert(filler[i]), OK);
    }

    EXPECT_EQ(input(lp.remote.syn(1000)), OK);
    uint32_t iss = sent_seq(lp, 0);
    EXPECT_EQ(input(lp.remote.ack(1001, iss + 1)), ERR_FULL);
    EXPECT_EQ(lp.link.frames_sent(), 1u);

    rc::strong_ref<record> rec = lookup(key_of(lp.remote));
    ASSERT_TRUE(rec);
    EXPECT_EQ(rec->kind, record_kind::request);
    EXPECT_TRUE(static_cast<const tcp_request*>(rec.ptr())->timer_armed);
    EXPECT_EQ(on.request_count(), 1);

    // The timer still retransmits, so the peer answers again once there is room
    advance_and_fire(TIMEOUT_INIT_NS);
    EXPECT_EQ(lp.link.frames_sent(), 2u);
    EXPECT_EQ(sent_tcp(lp.link, 1)->flags, FLAG_SYN | FLAG_ACK);

    EXPECT_EQ(remove(filler[0]), OK);
    EXPECT_EQ(input(lp.remote.ack(1001, iss + 1)), OK);
    EXPECT_EQ(lookup(key_of(lp.remote))->kind, record_kind::connection);
    EXPECT_EQ(on.request_count(), 0);

    for (size_t i = 1; i < MAX_CONNECTIONS; i++) {
        EXPECT_EQ(remove(filler[i]), OK);
    }

    for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
        if (filler[i]->release()) {
            record::ref_destroy(filler[i]);
        }
    }

    rc::strong_ref<tcp_conn> conn = pop_accepted(on.listener);
    ASSERT_TRUE(conn);
    abort_connection(conn.ptr());
}

// An active open toward the harness peer, under the fake clock
struct connecting {
    rc::strong_ref<tcp_conn> conn;

    explicit connecting(linked_peer& lp) {
        g_fake_now = clock::now_ns();
        __dbg_test_set_clock(fake_clock);
        tuple key = {lp.remote.host, lp.remote.addr, lp.remote.host_port, lp.remote.port};
        open_active(key, &lp.link, &conn);
    }

    ~connecting() {
        if (conn) {
            abort_connection(conn.ptr());
        }

        __dbg_test_set_clock(nullptr);
    }
};

static tcp_options peer_synack_options() {
    tcp_options opts;
    opts.mss = 1400;
    opts.sack_permitted = true;
    opts.has_timestamps = true;
    opts.ts_val = 555;
    opts.window_scale = 3;
    return opts;
}

TEST(tcp_handshake, active_open_sends_a_syn_with_every_option) {
    linked_peer lp;
    connecting active(lp);
    ASSERT_TRUE(active.conn);
    ASSERT_EQ(lp.link.frames_sent(), 1u);

    const tcp_header* hdr = sent_tcp(lp.link, 0);
    EXPECT_EQ(hdr->flags, FLAG_SYN);
    EXPECT_EQ(ntohl(hdr->seq), active.conn->iss);
    EXPECT_EQ(ntohl(hdr->ack), 0u);
    EXPECT_EQ(ntohs(hdr->window), RCV_WND_INITIAL);
    EXPECT_EQ(ntohs(hdr->src_port), 5000);
    EXPECT_EQ(ntohs(hdr->dst_port), 40000);

    tcp_options opts = synack_options(lp, 0);
    EXPECT_EQ(opts.mss, 1460);
    EXPECT_TRUE(opts.sack_permitted);
    EXPECT_TRUE(opts.has_timestamps);
    EXPECT_EQ(opts.window_scale, 5);

    EXPECT_EQ(active.conn->state, tcp_state::syn_sent);
    EXPECT_EQ(active.conn->snd_una, active.conn->iss);
    EXPECT_EQ(active.conn->snd_nxt, active.conn->iss + 1);
    EXPECT_EQ(active.conn->send_timer_kind, timer_kind::rto);
    rc::strong_ref<record> rec = lookup(active.conn->key);
    ASSERT_TRUE(rec);
    EXPECT_EQ(rec->kind, record_kind::connection);
}

TEST(tcp_handshake, syn_ack_completes_the_active_open_and_is_acknowledged) {
    linked_peer lp;
    connecting active(lp);
    uint32_t iss = active.conn->iss;

    EXPECT_EQ(input(lp.remote.segment(FLAG_SYN | FLAG_ACK, 7000, iss + 1, peer_synack_options())), OK);
    ASSERT_EQ(lp.link.frames_sent(), 2u);

    EXPECT_EQ(active.conn->state, tcp_state::established);
    EXPECT_EQ(active.conn->irs, 7000u);
    EXPECT_EQ(active.conn->rcv_nxt, 7001u);
    EXPECT_EQ(active.conn->snd_una, iss + 1);
    EXPECT_EQ(active.conn->snd_wnd, 65535u);
    EXPECT_EQ(active.conn->snd_wscale, 3);
    EXPECT_EQ(active.conn->rcv_wscale, 5);
    EXPECT_EQ(active.conn->snd_mss, 1400);
    EXPECT_TRUE(active.conn->sack_ok);
    EXPECT_TRUE(active.conn->ts_ok);
    EXPECT_EQ(active.conn->ts_recent, 555u);
    EXPECT_EQ(active.conn->send_timer_kind, timer_kind::none);

    const tcp_header* ack = sent_tcp(lp.link, 1);
    EXPECT_EQ(ack->flags, FLAG_ACK);
    EXPECT_EQ(ntohl(ack->seq), iss + 1);
    EXPECT_EQ(ntohl(ack->ack), 7001u);
    EXPECT_EQ(ntohs(ack->window), RCV_WND_INITIAL >> 5);
    tcp_options opts;
    parse_options(ack, &opts);
    EXPECT_TRUE(opts.has_timestamps);
    EXPECT_EQ(opts.ts_ecr, 555u);
    EXPECT_EQ(opts.mss, MSS_NONE);
}

TEST(tcp_handshake, syn_ack_without_options_leaves_them_off) {
    linked_peer lp;
    connecting active(lp);

    EXPECT_EQ(input(lp.remote.segment(FLAG_SYN | FLAG_ACK, 7000, active.conn->iss + 1)), OK);
    ASSERT_EQ(lp.link.frames_sent(), 2u);

    EXPECT_EQ(active.conn->state, tcp_state::established);
    EXPECT_FALSE(active.conn->wscale_ok);
    EXPECT_EQ(active.conn->rcv_wscale, 0);
    EXPECT_FALSE(active.conn->ts_ok);
    EXPECT_FALSE(active.conn->sack_ok);
    EXPECT_EQ(active.conn->snd_mss, DEFAULT_MSS);
    EXPECT_EQ(ntohs(sent_tcp(lp.link, 1)->window), RCV_WND_INITIAL);
    EXPECT_EQ(sent_tcp(lp.link, 1)->data_offset(), MIN_DATA_OFFSET);
}

TEST(tcp_handshake, syn_ack_with_a_wrong_acknowledgment_is_reset) {
    linked_peer lp;
    connecting active(lp);
    uint32_t iss = active.conn->iss;

    EXPECT_EQ(input(lp.remote.segment(FLAG_SYN | FLAG_ACK, 7000, iss + 5)), OK);
    ASSERT_EQ(lp.link.frames_sent(), 2u);
    EXPECT_EQ(sent_tcp(lp.link, 1)->flags, FLAG_RST);
    EXPECT_EQ(sent_seq(lp, 1), iss + 5);
    EXPECT_EQ(active.conn->state, tcp_state::syn_sent);
}

TEST(tcp_handshake, reset_refuses_the_connection_only_with_an_acceptable_ack) {
    linked_peer lp;
    connecting active(lp);
    uint32_t iss = active.conn->iss;

    EXPECT_EQ(input(lp.remote.rst(7000)), OK);
    EXPECT_EQ(active.conn->state, tcp_state::syn_sent);

    EXPECT_EQ(input(lp.remote.segment(FLAG_RST | FLAG_ACK, 0, iss + 1)), OK);
    EXPECT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(active.conn->state, tcp_state::closed);
    EXPECT_EQ(active.conn->pending_error, resource::ERR_CONNREFUSED);
    EXPECT_FALSE(lookup(active.conn->key));
}

TEST(tcp_handshake, syn_is_retransmitted_with_backoff_until_the_connection_times_out) {
    linked_peer lp;
    connecting active(lp);
    uint32_t iss = active.conn->iss;

    uint64_t backoff = TIMEOUT_INIT_NS;
    for (size_t attempt = 1; attempt <= SYN_RETRIES; attempt++) {
        advance_and_fire(backoff);
        ASSERT_EQ(lp.link.frames_sent(), attempt + 1);
        EXPECT_EQ(sent_tcp(lp.link, attempt)->flags, FLAG_SYN);
        EXPECT_EQ(sent_seq(lp, attempt), iss);
        backoff *= 2;
    }

    EXPECT_EQ(active.conn->state, tcp_state::syn_sent);
    advance_and_fire(backoff);
    EXPECT_EQ(lp.link.frames_sent(), static_cast<size_t>(SYN_RETRIES) + 1);
    EXPECT_EQ(active.conn->state, tcp_state::closed);
    EXPECT_EQ(active.conn->pending_error, resource::ERR_TIMEDOUT);
    EXPECT_FALSE(lookup(active.conn->key));
}

TEST(tcp_handshake, simultaneous_open_answers_with_a_syn_ack_and_completes_on_the_ack) {
    linked_peer lp;
    connecting active(lp);
    uint32_t iss = active.conn->iss;

    EXPECT_EQ(input(lp.remote.syn(9000)), OK);
    ASSERT_EQ(lp.link.frames_sent(), 2u);
    EXPECT_EQ(active.conn->state, tcp_state::syn_rcvd);
    EXPECT_EQ(sent_tcp(lp.link, 1)->flags, FLAG_SYN | FLAG_ACK);
    EXPECT_EQ(sent_seq(lp, 1), iss);
    EXPECT_EQ(ntohl(sent_tcp(lp.link, 1)->ack), 9001u);

    EXPECT_EQ(input(lp.remote.ack(9001, iss + 1)), OK);
    EXPECT_EQ(active.conn->state, tcp_state::established);
    EXPECT_EQ(active.conn->snd_una, iss + 1);
    EXPECT_EQ(active.conn->send_timer_kind, timer_kind::none);
    EXPECT_EQ(lp.link.frames_sent(), 2u);
}

TEST(tcp_handshake, syn_ack_arriving_after_the_open_timed_out_is_ignored) {
    linked_peer lp;
    connecting active(lp);
    uint32_t iss = active.conn->iss;

    uint64_t backoff = TIMEOUT_INIT_NS;
    for (size_t attempt = 0; attempt <= SYN_RETRIES; attempt++) {
        advance_and_fire(backoff);
        backoff *= 2;
    }

    ASSERT_EQ(active.conn->state, tcp_state::closed);
    size_t frames = lp.link.frames_sent();

    // The connection is gone, so the peer's SYN-ACK meets the CLOSED state and a reset
    EXPECT_EQ(input(lp.remote.segment(FLAG_SYN | FLAG_ACK, 7000, iss + 1)), OK);
    ASSERT_EQ(lp.link.frames_sent(), frames + 1);
    EXPECT_EQ(sent_tcp(lp.link, frames)->flags, FLAG_RST);
    EXPECT_EQ(sent_seq(lp, frames), iss + 1);
    EXPECT_EQ(active.conn->state, tcp_state::closed);
    EXPECT_EQ(active.conn->pending_error, resource::ERR_TIMEDOUT);
    EXPECT_FALSE(lookup(active.conn->key));
}

TEST(tcp_handshake, a_late_failure_leaves_an_established_connection_alone) {
    linked_peer lp;
    connecting active(lp);

    EXPECT_EQ(input(lp.remote.segment(FLAG_SYN | FLAG_ACK, 7000, active.conn->iss + 1)), OK);
    ASSERT_EQ(active.conn->state, tcp_state::established);

    fail_connection(active.conn.ptr(), resource::ERR_TIMEDOUT);
    EXPECT_EQ(active.conn->state, tcp_state::established);
    EXPECT_EQ(active.conn->pending_error, resource::OK);
    EXPECT_TRUE(lookup(active.conn->key));
}

TEST(tcp_handshake, only_a_bare_syn_reaches_the_listener) {
    linked_peer lp;
    listening on(8);

    EXPECT_EQ(input(lp.remote.ack(1000, 1)), OK);
    EXPECT_EQ(input(lp.remote.segment(FLAG_SYN | FLAG_ACK, 1000, 1)), OK);
    ASSERT_EQ(lp.link.frames_sent(), 2u);

    EXPECT_EQ(sent_tcp(lp.link, 0)->flags, FLAG_RST);
    EXPECT_EQ(sent_tcp(lp.link, 1)->flags, FLAG_RST);
    EXPECT_FALSE(lookup(key_of(lp.remote)));
    EXPECT_EQ(on.request_count(), 0);
}
