#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "harness.h"
#include "net/tcp/conn.h"
#include "net/tcp/listen.h"
#include "net/tcp/seq.h"
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
    EXPECT_EQ(ntohs(hdr->window), RCV_BUF_INITIAL);
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
