#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "net/tcp/conn.h"
#include "net/tcp/listen.h"
#include "net/tcp/timewait.h"
#include "net/net.h"
#include "mm/heap.h"
#include "timer/timer.h"

TEST_SUITE(tcp_tables);

using namespace net;
using namespace net::tcp;

static const ipv4::ipv4_addr g_host   = {{10, 0, 2, 15}};
static const ipv4::ipv4_addr g_peer   = {{10, 0, 2, 2}};
static const ipv4::ipv4_addr g_other  = {{10, 0, 2, 3}};

// Stand-ins for interfaces, only ever compared by address
static int g_iface_a;
static int g_iface_b;
static interface* const IFACE_A = reinterpret_cast<interface*>(&g_iface_a);
static interface* const IFACE_B = reinterpret_cast<interface*>(&g_iface_b);

static tuple key_for(uint16_t local_port, uint16_t remote_port) {
    return tuple{g_host, g_peer, local_port, remote_port};
}

// Drops the creator's reference, leaving only the table's if it holds one
static void drop(record* rec) {
    if (rec->release()) {
        record::ref_destroy(rec);
    }
}

static void drop(tcp_listener* listener) {
    if (listener->release()) {
        tcp_listener::ref_destroy(listener);
    }
}

static tcp_request* alloc_request(const tuple& key) {
    tcp_request* request = heap::ualloc_new<tcp_request>();
    request->kind = record_kind::request;
    request->key = key;
    timer::init_deadline_timer(&request->timer, nullptr);

    return request;
}

static tcp_listener* listener_on(const ipv4::ipv4_addr& addr, uint16_t port, interface* iface,
                                 bool reuseaddr) {
    tcp_listener* listener = alloc_listener(addr, port);
    listener->iface = iface;
    listener->reuseaddr = reuseaddr;

    return listener;
}

TEST(tcp_tables, empty_table_finds_nothing) {
    EXPECT_FALSE(lookup(key_for(5000, 40000)));
    EXPECT_EQ(record_count(record_kind::connection), 0u);
}

TEST(tcp_tables, inserted_connection_is_found_by_its_tuple_only) {
    tcp_conn* conn = alloc_conn(key_for(5000, 40000), IFACE_A);
    ASSERT_NOT_NULL(conn);
    EXPECT_EQ(conn->state, tcp_state::closed);
    EXPECT_EQ(conn->iface, IFACE_A);

    ASSERT_EQ(insert(conn), OK);
    EXPECT_EQ(record_count(record_kind::connection), 1u);

    {
        rc::strong_ref<record> found = lookup(key_for(5000, 40000));
        ASSERT_TRUE(found);
        EXPECT_EQ(found.ptr(), static_cast<record*>(conn));
        EXPECT_EQ(found->kind, record_kind::connection);
    }

    EXPECT_FALSE(lookup(key_for(5000, 40001)));
    EXPECT_FALSE(lookup(key_for(5001, 40000)));
    EXPECT_FALSE(lookup(tuple{g_host, g_other, 5000, 40000}));
    EXPECT_FALSE(lookup(tuple{g_other, g_peer, 5000, 40000}));

    EXPECT_EQ(remove(conn), OK);
    EXPECT_FALSE(lookup(key_for(5000, 40000)));
    EXPECT_EQ(record_count(record_kind::connection), 0u);
    drop(conn);
}

TEST(tcp_tables, a_taken_tuple_refuses_a_second_record) {
    tcp_conn* first = alloc_conn(key_for(5000, 40000), IFACE_A);
    tcp_conn* second = alloc_conn(key_for(5000, 40000), IFACE_A);
    ASSERT_EQ(insert(first), OK);

    EXPECT_EQ(insert(second), ERR_IN_USE);
    EXPECT_EQ(insert(first), ERR_IN_USE);
    EXPECT_EQ(record_count(record_kind::connection), 1u);

    EXPECT_EQ(remove(first), OK);
    EXPECT_EQ(remove(first), ERR_NOT_FOUND);
    EXPECT_EQ(remove(second), ERR_NOT_FOUND);
    drop(first);
    drop(second);
}

TEST(tcp_tables, table_holds_its_own_reference) {
    tcp_conn* conn = alloc_conn(key_for(5000, 40000), IFACE_A);
    ASSERT_EQ(insert(conn), OK);

    // The creator lets go, the table keeps the record alive and findable
    drop(conn);
    rc::strong_ref<record> found = lookup(key_for(5000, 40000));
    ASSERT_TRUE(found);
    EXPECT_EQ(found->key.remote_port, 40000);

    EXPECT_EQ(remove(found.ptr()), OK);
    EXPECT_FALSE(lookup(key_for(5000, 40000)));
}

TEST(tcp_tables, each_kind_has_its_own_capacity) {
    tcp_conn* conns[MAX_CONNECTIONS + 1];
    for (size_t i = 0; i <= MAX_CONNECTIONS; i++) {
        conns[i] = alloc_conn(key_for(5000, static_cast<uint16_t>(40000 + i)), IFACE_A);
        ASSERT_NOT_NULL(conns[i]);
    }

    for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
        ASSERT_EQ(insert(conns[i]), OK);
    }

    EXPECT_EQ(insert(conns[MAX_CONNECTIONS]), ERR_FULL);
    EXPECT_EQ(record_count(record_kind::connection), MAX_CONNECTIONS);

    // A request is another kind and still fits
    tcp_request* request = alloc_request(key_for(5001, 40000));
    EXPECT_EQ(insert(request), OK);
    EXPECT_EQ(record_count(record_kind::request), 1u);
    EXPECT_EQ(remove(request), OK);
    drop(request);

    for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
        EXPECT_EQ(remove(conns[i]), OK);
    }

    for (size_t i = 0; i <= MAX_CONNECTIONS; i++) {
        drop(conns[i]);
    }

    EXPECT_EQ(record_count(record_kind::connection), 0u);
}

TEST(tcp_tables, replace_swaps_a_request_for_a_connection_under_one_key) {
    tcp_request* request = alloc_request(key_for(5000, 40000));
    tcp_conn* conn = alloc_conn(key_for(5000, 40000), IFACE_A);
    ASSERT_EQ(insert(request), OK);

    EXPECT_EQ(replace(request, conn), OK);
    EXPECT_EQ(record_count(record_kind::request), 0u);
    EXPECT_EQ(record_count(record_kind::connection), 1u);

    rc::strong_ref<record> found = lookup(key_for(5000, 40000));
    ASSERT_TRUE(found);
    EXPECT_EQ(found->kind, record_kind::connection);
    EXPECT_EQ(found.ptr(), static_cast<record*>(conn));

    EXPECT_EQ(remove(request), ERR_NOT_FOUND);
    EXPECT_EQ(replace(request, conn), ERR_NOT_FOUND);
    found.reset();
    EXPECT_EQ(remove(conn), OK);
    drop(request);
    drop(conn);
}

TEST(tcp_tables, replace_requires_the_same_key) {
    tcp_request* request = alloc_request(key_for(5000, 40000));
    tcp_conn* conn = alloc_conn(key_for(5000, 40001), IFACE_A);
    ASSERT_EQ(insert(request), OK);

    EXPECT_EQ(replace(request, conn), ERR_NOT_FOUND);
    EXPECT_EQ(record_count(record_kind::request), 1u);

    EXPECT_EQ(remove(request), OK);
    drop(request);
    drop(conn);
}

TEST(tcp_tables, ephemeral_ports_come_from_the_range_and_avoid_taken_ports) {
    uint16_t first = 0;
    uint16_t second = 0;
    ASSERT_EQ(take_ephemeral_port(&first), OK);
    ASSERT_EQ(take_ephemeral_port(&second), OK);
    EXPECT_TRUE(first >= EPHEMERAL_PORT_MIN);
    EXPECT_TRUE(second >= EPHEMERAL_PORT_MIN);
    EXPECT_TRUE(first != second);

    // Ports are handed out in sequence, so occupying the one that comes next
    // forces the allocator to step over it
    uint16_t taken = second == EPHEMERAL_PORT_MAX ? EPHEMERAL_PORT_MIN : second + 1;
    uint16_t after_taken = taken == EPHEMERAL_PORT_MAX ? EPHEMERAL_PORT_MIN : taken + 1;
    tcp_conn* conn = alloc_conn(tuple{g_host, g_peer, taken, 40000}, IFACE_A);
    ASSERT_EQ(insert(conn), OK);
    EXPECT_TRUE(is_local_port_taken(taken));

    uint16_t third = 0;
    ASSERT_EQ(take_ephemeral_port(&third), OK);
    EXPECT_EQ(third, after_taken);

    EXPECT_EQ(remove(conn), OK);
    EXPECT_FALSE(is_local_port_taken(taken));
    drop(conn);
}

TEST(tcp_tables, listener_port_counts_as_taken) {
    tcp_listener* listener = listener_on(ipv4::UNSPECIFIED_ADDR, 5000, nullptr, false);
    ASSERT_EQ(listener_insert(listener), OK);

    EXPECT_TRUE(is_listener_port(5000));
    EXPECT_TRUE(is_local_port_taken(5000));
    EXPECT_FALSE(is_local_port_taken(5001));

    EXPECT_EQ(listener_remove(listener), OK);
    EXPECT_EQ(listener_remove(listener), ERR_NOT_FOUND);
    EXPECT_FALSE(is_local_port_taken(5000));
    drop(listener);
}

TEST(tcp_tables, listeners_share_a_port_only_across_interfaces_or_with_reuseaddr) {
    tcp_listener* any = listener_on(ipv4::UNSPECIFIED_ADDR, 5000, nullptr, false);
    tcp_listener* same = listener_on(ipv4::UNSPECIFIED_ADDR, 5000, nullptr, false);
    tcp_listener* specific = listener_on(g_host, 5000, nullptr, false);
    ASSERT_EQ(listener_insert(any), OK);

    EXPECT_EQ(listener_insert(same), ERR_IN_USE);
    EXPECT_EQ(listener_insert(specific), ERR_IN_USE);

    tcp_listener* other_iface = listener_on(g_host, 5000, IFACE_B, false);
    tcp_listener* any_on_a = listener_on(ipv4::UNSPECIFIED_ADDR, 5001, IFACE_A, false);
    tcp_listener* specific_on_b = listener_on(g_host, 5001, IFACE_B, false);
    EXPECT_EQ(listener_insert(other_iface), ERR_IN_USE);
    EXPECT_EQ(listener_insert(any_on_a), OK);
    EXPECT_EQ(listener_insert(specific_on_b), OK);

    tcp_listener* shared_any = listener_on(ipv4::UNSPECIFIED_ADDR, 5002, nullptr, true);
    tcp_listener* shared_specific = listener_on(g_host, 5002, nullptr, true);
    tcp_listener* shared_twice = listener_on(g_host, 5002, nullptr, true);
    EXPECT_EQ(listener_insert(shared_any), OK);
    EXPECT_EQ(listener_insert(shared_specific), OK);
    EXPECT_EQ(listener_insert(shared_twice), ERR_IN_USE);

    tcp_listener* inserted[] = {any, any_on_a, specific_on_b, shared_any, shared_specific};
    for (tcp_listener* listener : inserted) {
        EXPECT_EQ(listener_remove(listener), OK);
    }

    tcp_listener* all[] = {any, same, specific, other_iface, any_on_a, specific_on_b,
                           shared_any, shared_specific, shared_twice};
    for (tcp_listener* listener : all) {
        drop(listener);
    }
}

TEST(tcp_tables, lookup_prefers_the_exact_address_and_honors_the_interface) {
    tcp_listener* any = listener_on(ipv4::UNSPECIFIED_ADDR, 5000, nullptr, true);
    tcp_listener* specific = listener_on(g_host, 5000, nullptr, true);
    tcp_listener* pinned = listener_on(ipv4::UNSPECIFIED_ADDR, 5001, IFACE_A, false);
    ASSERT_EQ(listener_insert(any), OK);
    ASSERT_EQ(listener_insert(specific), OK);
    ASSERT_EQ(listener_insert(pinned), OK);

    EXPECT_EQ(listener_lookup(g_host, 5000, IFACE_A).ptr(), specific);
    EXPECT_EQ(listener_lookup(g_other, 5000, IFACE_A).ptr(), any);
    EXPECT_FALSE(listener_lookup(g_host, 5002, IFACE_A));

    EXPECT_EQ(listener_lookup(g_host, 5001, IFACE_A).ptr(), pinned);
    EXPECT_FALSE(listener_lookup(g_host, 5001, IFACE_B));

    tcp_listener* all[] = {any, specific, pinned};
    for (tcp_listener* listener : all) {
        EXPECT_EQ(listener_remove(listener), OK);
        drop(listener);
    }
}

TEST(tcp_tables, listener_table_has_a_capacity) {
    tcp_listener* listeners[MAX_LISTENERS + 1];
    for (size_t i = 0; i <= MAX_LISTENERS; i++) {
        listeners[i] = listener_on(ipv4::UNSPECIFIED_ADDR, static_cast<uint16_t>(6000 + i), nullptr, false);
        ASSERT_NOT_NULL(listeners[i]);
    }

    for (size_t i = 0; i < MAX_LISTENERS; i++) {
        ASSERT_EQ(listener_insert(listeners[i]), OK);
    }

    EXPECT_EQ(listener_insert(listeners[MAX_LISTENERS]), ERR_FULL);

    for (size_t i = 0; i < MAX_LISTENERS; i++) {
        EXPECT_EQ(listener_remove(listeners[i]), OK);
    }

    for (size_t i = 0; i <= MAX_LISTENERS; i++) {
        drop(listeners[i]);
    }
}
