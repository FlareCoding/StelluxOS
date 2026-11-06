#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "drivers/virtio/virtqueue.h"

namespace drivers::virtio {

// Reads the descriptor table and the available ring as the device would
struct virtqueue_test_access {
    static uint64_t address(const virtqueue& queue, uint16_t index) { return queue.m_desc[index].addr; }
    static uint32_t length(const virtqueue& queue, uint16_t index) { return queue.m_desc[index].len; }
    static uint16_t flags(const virtqueue& queue, uint16_t index) { return queue.m_desc[index].flags; }
    static uint16_t next(const virtqueue& queue, uint16_t index) { return queue.m_desc[index].next; }

    static uint16_t avail_idx(const virtqueue& queue) { return queue.m_avail->idx; }

    static uint16_t newest_avail(const virtqueue& queue) {
        return queue.m_avail->ring[(queue.m_avail->idx - 1) % queue.m_size];
    }
};

} // namespace drivers::virtio

using drivers::virtio::VRING_DESC_F_NEXT;
using drivers::virtio::VRING_DESC_F_WRITE;
using drivers::virtio::chain_buffer;
using drivers::virtio::virtqueue;
using drivers::virtio::virtqueue_test_access;

TEST_SUITE(virtqueue_test);

constexpr uint16_t QUEUE_SIZE = 8;

// A request whose response spans two device-writable buffers, every field distinct
constexpr chain_buffer THREE_BUFFERS[] = {
    {0x10000, 56, 0},
    {0x22000, 4096, VRING_DESC_F_WRITE},
    {0x31000, 24, VRING_DESC_F_WRITE},
};

// One queue serves every test, since a virtqueue never frees its rings. Each test frees what it adds.
static virtqueue g_queue;

static int32_t virtqueue_before_all() {
    return g_queue.init(QUEUE_SIZE);
}

BEFORE_ALL(virtqueue_test, virtqueue_before_all);

TEST(virtqueue_test, chain_links_its_buffers_in_order) {
    uint16_t free_before = g_queue.free_count();
    uint16_t avail_before = virtqueue_test_access::avail_idx(g_queue);

    int32_t head = g_queue.add_buf_chain(THREE_BUFFERS, 3);
    ASSERT_TRUE(head >= 0);

    uint16_t index = static_cast<uint16_t>(head);
    for (uint16_t i = 0; i < 3; i++) {
        bool is_last = i == 2;
        uint16_t expected_flags = is_last ? THREE_BUFFERS[i].flags : THREE_BUFFERS[i].flags | VRING_DESC_F_NEXT;

        EXPECT_EQ(virtqueue_test_access::address(g_queue, index), THREE_BUFFERS[i].phys);
        EXPECT_EQ(virtqueue_test_access::length(g_queue, index), THREE_BUFFERS[i].len);
        EXPECT_EQ(virtqueue_test_access::flags(g_queue, index), expected_flags);

        index = virtqueue_test_access::next(g_queue, index);
    }

    EXPECT_EQ(g_queue.free_count(), free_before - 3);
    EXPECT_EQ(virtqueue_test_access::avail_idx(g_queue), static_cast<uint16_t>(avail_before + 1));
    EXPECT_EQ(virtqueue_test_access::newest_avail(g_queue), static_cast<uint16_t>(head));

    g_queue.free_desc(static_cast<uint16_t>(head));
    EXPECT_EQ(g_queue.free_count(), free_before);
}

TEST(virtqueue_test, chain_needs_a_free_descriptor_per_buffer) {
    int32_t held[QUEUE_SIZE] = {};
    uint16_t held_count = 0;
    while (g_queue.free_count() > 2) {
        held[held_count++] = g_queue.add_buf(0x40000, 8, 0);
    }

    uint16_t avail_before = virtqueue_test_access::avail_idx(g_queue);

    EXPECT_EQ(g_queue.add_buf_chain(THREE_BUFFERS, 3), -1);
    EXPECT_EQ(g_queue.free_count(), static_cast<uint16_t>(2));
    EXPECT_EQ(virtqueue_test_access::avail_idx(g_queue), avail_before);

    g_queue.free_desc(static_cast<uint16_t>(held[--held_count]));
    int32_t head = g_queue.add_buf_chain(THREE_BUFFERS, 3);
    EXPECT_TRUE(head >= 0);
    EXPECT_EQ(g_queue.free_count(), static_cast<uint16_t>(0));

    if (head >= 0) {
        g_queue.free_desc(static_cast<uint16_t>(head));
    }

    for (uint16_t i = 0; i < held_count; i++) {
        g_queue.free_desc(static_cast<uint16_t>(held[i]));
    }
}

TEST(virtqueue_test, empty_chain_refused) {
    uint16_t free_before = g_queue.free_count();
    uint16_t avail_before = virtqueue_test_access::avail_idx(g_queue);

    EXPECT_EQ(g_queue.add_buf_chain(THREE_BUFFERS, 0), -1);
    EXPECT_EQ(g_queue.free_count(), free_before);
    EXPECT_EQ(virtqueue_test_access::avail_idx(g_queue), avail_before);
}
