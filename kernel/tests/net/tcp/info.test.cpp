#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "harness.h"
#include "net/tcp/info.h"
#include "net/tcp/conn.h"
#include "net/tcp/listen.h"
#include "net/tcp/timewait.h"
#include "resource/resource.h"
#include "clock/clock.h"
#include "timer/timer.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(tcp_info);

using namespace net;
using namespace net::tcp;

constexpr uint32_t PEER_ISS    = 3000;
constexpr uint32_t HOST_ADDR   = 0x0A00020F; // 10.0.2.15
constexpr uint32_t REMOTE_ADDR = 0x0A000202; // 10.0.2.2

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

static void release(record* rec) {
    if (rec->release()) {
        record::ref_destroy(rec);
    }
}

// A connection this host opened, established with a peer that scales by 3,
// stamps its segments, and offers an MSS of 1400
struct inspected {
    rc::strong_ref<tcp_conn> conn;
    linked_peer&             lp;

    explicit inspected(linked_peer& link) : lp(link) {
        g_fake_now = clock::now_ns();
        __dbg_test_set_clock(fake_clock);

        open_active(key_of(lp.remote), &lp.link, &conn);

        tcp_options opts;
        opts.mss = 1400;
        opts.has_timestamps = true;
        opts.ts_val = 77;
        opts.window_scale = 3;
        input(lp.remote.segment(FLAG_SYN | FLAG_ACK, PEER_ISS, conn->iss + 1, opts));
        lp.link.clear_frames();
    }

    ~inspected() {
        abort_connection(conn.ptr());
        if (lookup(key_of(lp.remote))) {
            advance_and_fire(TIMEWAIT_LEN_NS);
        }

        __dbg_test_set_clock(nullptr);
    }
};

// A listener on port 5000 for the wildcard address, released with the test
struct listening_for_info {
    tcp_listener* listener;

    explicit listening_for_info(uint16_t backlog) {
        listener = alloc_listener(endpoint{ipv4::UNSPECIFIED_ADDR, 5000, nullptr, false});
        listener->backlog = backlog;
        listener_insert(listener);
    }

    ~listening_for_info() {
        listener_close(listener);
        if (listener->release()) {
            tcp_listener::ref_destroy(listener);
        }
    }
};

TEST(tcp_info, a_listener_is_described_and_collected) {
    listening_for_info on(8);

    tcp_record entry;
    describe_listener(on.listener, &entry);
    EXPECT_EQ(entry.kind, INFO_KIND_LISTENER);
    EXPECT_EQ(entry.state, static_cast<uint8_t>(tcp_state::listen));
    EXPECT_EQ(entry.local_addr, 0u);
    EXPECT_EQ(entry.local_port, 5000);
    EXPECT_EQ(entry.remote_port, 0);
    EXPECT_EQ(entry.backlog, 8);
    EXPECT_EQ(entry.requests, 0);
    EXPECT_EQ(entry.accepted, 0);
    EXPECT_EQ(entry.iface[0], '\0');
    EXPECT_EQ(entry.timer_kind, INFO_TIMER_NONE);

    tcp_listener* found[MAX_LISTENERS];
    size_t total = 0;
    uint32_t refs_before = on.listener->ref_count();
    ASSERT_EQ(collect_listeners(found, MAX_LISTENERS, &total), 1u);
    EXPECT_EQ(total, 1u);
    EXPECT_EQ(found[0], on.listener);
    EXPECT_EQ(found[0]->ref_count(), refs_before + 1);
    EXPECT_FALSE(found[0]->release());

    EXPECT_EQ(collect_listeners(nullptr, 0, &total), 0u);
    EXPECT_EQ(total, 1u);
}

TEST(tcp_info, an_established_connection_is_described) {
    linked_peer lp;
    inspected c(lp);

    tcp_record entry;
    describe_record(c.conn.ptr(), &entry);
    EXPECT_EQ(entry.kind, INFO_KIND_CONNECTION);
    EXPECT_EQ(entry.state, static_cast<uint8_t>(tcp_state::established));
    EXPECT_EQ(entry.local_addr, HOST_ADDR);
    EXPECT_EQ(entry.remote_addr, REMOTE_ADDR);
    EXPECT_EQ(entry.local_port, lp.remote.host_port);
    EXPECT_EQ(entry.remote_port, lp.remote.port);
    EXPECT_TRUE(string::strcmp(entry.iface, lp.link.name()) == 0);
    EXPECT_EQ(entry.snd_una, c.conn->iss + 1);
    EXPECT_EQ(entry.snd_nxt, c.conn->iss + 1);
    EXPECT_EQ(entry.rcv_nxt, PEER_ISS + 1);
    EXPECT_EQ(entry.snd_wnd, 65535u);
    EXPECT_EQ(entry.rcv_wnd, c.conn->rcv_wnd);
    EXPECT_EQ(entry.snd_mss, 1400);
    EXPECT_EQ(entry.snd_wscale, 3);
    EXPECT_EQ(entry.rcv_wscale, c.conn->rcv_wscale);
    EXPECT_EQ(entry.flags, INFO_TIMESTAMPS | INFO_WINDOW_SCALE);
    EXPECT_EQ(entry.retransmits, 0);
    EXPECT_EQ(entry.timer_kind, INFO_TIMER_NONE);
    EXPECT_EQ(entry.timer_ms, 0u);
    EXPECT_EQ(entry.error, 0);
}

TEST(tcp_info, a_request_and_a_time_wait_record_are_described_with_their_timers) {
    linked_peer lp;
    inspected c(lp);

    close_connection(c.conn.ptr());
    EXPECT_EQ(input(lp.remote.segment(FLAG_FIN | FLAG_ACK, PEER_ISS + 1, c.conn->iss + 2)), OK);
    rc::strong_ref<record> tw = lookup(key_of(lp.remote));
    ASSERT_TRUE(tw);

    tcp_record entry;
    describe_record(tw.ptr(), &entry);
    EXPECT_EQ(entry.kind, INFO_KIND_TIMEWAIT);
    EXPECT_EQ(entry.state, static_cast<uint8_t>(tcp_state::time_wait));
    EXPECT_EQ(entry.snd_nxt, c.conn->iss + 2);
    EXPECT_EQ(entry.rcv_nxt, PEER_ISS + 2);
    EXPECT_EQ(entry.flags, INFO_TIMESTAMPS);
    EXPECT_EQ(entry.timer_kind, INFO_TIMER_TIMEWAIT);
    EXPECT_EQ(entry.timer_ms, TIMEWAIT_LEN_NS / 1000000);
    tw = {};
    advance_and_fire(TIMEWAIT_LEN_NS);

    listening_for_info on(8);
    EXPECT_EQ(input(lp.remote.syn(PEER_ISS + 100)), OK);
    rc::strong_ref<record> request = lookup(key_of(lp.remote));
    ASSERT_TRUE(request);

    describe_record(request.ptr(), &entry);
    EXPECT_EQ(entry.kind, INFO_KIND_REQUEST);
    EXPECT_EQ(entry.state, static_cast<uint8_t>(tcp_state::syn_rcvd));
    EXPECT_EQ(entry.local_port, 5000);
    EXPECT_EQ(entry.remote_addr, REMOTE_ADDR);
    EXPECT_EQ(entry.rcv_nxt, PEER_ISS + 101);
    EXPECT_EQ(entry.snd_nxt, entry.snd_una + 1);
    EXPECT_EQ(entry.timer_kind, INFO_TIMER_RTO);
    EXPECT_EQ(entry.timer_ms, TIMEOUT_INIT_NS / 1000000);
    EXPECT_TRUE(string::strcmp(entry.iface, lp.link.name()) == 0);
}

TEST(tcp_info, collect_records_honors_its_limit_and_reports_the_total) {
    linked_peer lp;
    inspected c(lp);
    uint32_t refs_before = c.conn->ref_count();

    record* found[2] = {};
    size_t total = 0;
    ASSERT_EQ(collect_records(found, 2, &total), 1u);
    EXPECT_EQ(total, 1u);
    EXPECT_EQ(found[0], c.conn.ptr());
    EXPECT_EQ(c.conn->ref_count(), refs_before + 1);
    release(found[0]);
    EXPECT_EQ(c.conn->ref_count(), refs_before);

    EXPECT_EQ(collect_records(nullptr, 0, &total), 0u);
    EXPECT_EQ(total, 1u);
}

TEST(tcp_info, counters_follow_segments_resets_and_bad_checksums) {
    linked_peer lp;
    g_fake_now = clock::now_ns();
    __dbg_test_set_clock(fake_clock);

    tcp_counters before;
    describe_counters(&before);

    packet* bad = lp.remote.syn(PEER_ISS);
    reinterpret_cast<tcp_header*>(bad->data())->checksum ^= 0xFFFF;
    EXPECT_EQ(input(bad), ERR_INVALID);

    EXPECT_EQ(input(lp.remote.syn(PEER_ISS)), OK);
    EXPECT_EQ(input(lp.remote.rst(PEER_ISS)), OK);

    tcp_counters after;
    describe_counters(&after);
    EXPECT_EQ(after.segments_in, before.segments_in + 3);
    EXPECT_EQ(after.checksum_failures, before.checksum_failures + 1);
    EXPECT_EQ(after.segments_out, before.segments_out + 1);
    EXPECT_EQ(after.rsts_sent, before.rsts_sent + 1);
    EXPECT_EQ(after.rsts_received, before.rsts_received + 1);
    EXPECT_EQ(after.connections, 0u);
    EXPECT_EQ(after.listeners, 0u);

    __dbg_test_set_clock(nullptr);
}

TEST(tcp_info, retransmissions_and_listen_drops_are_counted) {
    linked_peer lp;
    inspected c(lp);

    tcp_counters before;
    describe_counters(&before);

    close_connection(c.conn.ptr());
    advance_and_fire(TIMEOUT_MAX_NS);

    tcp_counters after;
    describe_counters(&after);
    EXPECT_EQ(after.retransmits, before.retransmits + 1);
    EXPECT_EQ(after.segments_out, before.segments_out + 2);
    EXPECT_EQ(after.connections, 1u);

    EXPECT_EQ(input(lp.remote.rst(PEER_ISS + 1)), OK);
    listening_for_info on(0);
    EXPECT_EQ(input(lp.remote.syn(PEER_ISS + 100)), OK);
    describe_counters(&after);
    EXPECT_EQ(after.listen_drops, before.listen_drops + 1);
    EXPECT_EQ(after.listeners, 1u);
    EXPECT_FALSE(lookup(key_of(lp.remote)));
}
