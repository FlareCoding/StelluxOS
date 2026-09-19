#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "net/tcp/byte_queue.h"

TEST_SUITE(tcp_byte_queue);

using namespace net::tcp;

constexpr size_t SCRATCH = 4 * CHUNK_SIZE;

static uint8_t g_src[SCRATCH];
static uint8_t g_dst[SCRATCH];

static uint8_t pattern(size_t index) {
    return static_cast<uint8_t>(index * 7 + 3);
}

// Fills g_src with the pattern for byte indices first .. first + len - 1
static const uint8_t* patterned(size_t first, size_t len) {
    for (size_t i = 0; i < len; i++) {
        g_src[i] = pattern(first + i);
    }

    return g_src;
}

static bool matches_pattern(const uint8_t* bytes, size_t first, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (bytes[i] != pattern(first + i)) {
            return false;
        }
    }

    return true;
}

// A queue that gives everything back when the test ends
struct scoped_queue {
    byte_queue q;

    explicit scoped_queue(size_t limit_chunks) { q.init(limit_chunks); }
    ~scoped_queue() { q.clear(); }
};

TEST(tcp_byte_queue, starts_empty_and_holds_no_chunk_until_appended) {
    size_t budget_before = budget_in_use();
    scoped_queue s(4);

    EXPECT_EQ(s.q.size(), 0u);
    EXPECT_EQ(s.q.chunk_count(), 0u);
    EXPECT_EQ(s.q.limit(), 4u);
    EXPECT_EQ(s.q.free_space(), 4 * CHUNK_PAYLOAD);
    EXPECT_EQ(budget_in_use(), budget_before);
    EXPECT_EQ(s.q.copy_out(0, g_dst, 10), 0u);
    EXPECT_EQ(s.q.consume(10), 0u);
}

TEST(tcp_byte_queue, append_fills_a_chunk_before_taking_the_next) {
    size_t budget_before = budget_in_use();
    scoped_queue s(4);

    EXPECT_EQ(s.q.append(patterned(0, CHUNK_PAYLOAD), CHUNK_PAYLOAD), CHUNK_PAYLOAD);
    EXPECT_EQ(s.q.chunk_count(), 1u);
    EXPECT_EQ(s.q.free_space(), 3 * CHUNK_PAYLOAD);
    EXPECT_EQ(budget_in_use(), budget_before + CHUNK_SIZE);

    EXPECT_EQ(s.q.append(patterned(CHUNK_PAYLOAD, 1), 1), 1u);
    EXPECT_EQ(s.q.chunk_count(), 2u);
    EXPECT_EQ(s.q.size(), CHUNK_PAYLOAD + 1);
    EXPECT_EQ(s.q.free_space(), 3 * CHUNK_PAYLOAD - 1);
    EXPECT_EQ(budget_in_use(), budget_before + 2 * CHUNK_SIZE);
}

TEST(tcp_byte_queue, copy_out_reads_across_chunk_boundaries_at_any_offset) {
    scoped_queue s(4);
    size_t total = 2 * CHUNK_PAYLOAD + CHUNK_PAYLOAD / 2;
    EXPECT_EQ(s.q.append(patterned(0, total), total), total);

    EXPECT_EQ(s.q.copy_out(0, g_dst, total), total);
    EXPECT_TRUE(matches_pattern(g_dst, 0, total));

    size_t offset = CHUNK_PAYLOAD - 5;
    EXPECT_EQ(s.q.copy_out(offset, g_dst, 10), 10u);
    EXPECT_TRUE(matches_pattern(g_dst, offset, 10));

    offset = CHUNK_PAYLOAD + 100;
    EXPECT_EQ(s.q.copy_out(offset, g_dst, CHUNK_PAYLOAD), CHUNK_PAYLOAD);
    EXPECT_TRUE(matches_pattern(g_dst, offset, CHUNK_PAYLOAD));

    EXPECT_EQ(s.q.copy_out(total - 3, g_dst, 100), 3u);
    EXPECT_TRUE(matches_pattern(g_dst, total - 3, 3));
    EXPECT_EQ(s.q.copy_out(total, g_dst, 1), 0u);
    EXPECT_EQ(s.q.size(), total);
}

TEST(tcp_byte_queue, consume_releases_emptied_chunks_and_keeps_the_rest_addressable) {
    size_t budget_before = budget_in_use();
    scoped_queue s(4);
    size_t total = 3 * CHUNK_PAYLOAD;
    EXPECT_EQ(s.q.append(patterned(0, total), total), total);

    EXPECT_EQ(s.q.consume(CHUNK_PAYLOAD + 10), CHUNK_PAYLOAD + 10);
    EXPECT_EQ(s.q.chunk_count(), 2u);
    EXPECT_EQ(s.q.size(), 2 * CHUNK_PAYLOAD - 10);
    EXPECT_EQ(budget_in_use(), budget_before + 2 * CHUNK_SIZE);
    EXPECT_EQ(s.q.copy_out(0, g_dst, 50), 50u);
    EXPECT_TRUE(matches_pattern(g_dst, CHUNK_PAYLOAD + 10, 50));

    EXPECT_EQ(s.q.consume(SCRATCH), 2 * CHUNK_PAYLOAD - 10);
    EXPECT_EQ(s.q.size(), 0u);
    EXPECT_EQ(s.q.chunk_count(), 0u);
    EXPECT_EQ(budget_in_use(), budget_before);

    EXPECT_EQ(s.q.append(patterned(500, 20), 20), 20u);
    EXPECT_EQ(s.q.copy_out(0, g_dst, 20), 20u);
    EXPECT_TRUE(matches_pattern(g_dst, 500, 20));
}

TEST(tcp_byte_queue, append_stops_at_the_chunk_limit_and_resumes_after_consume) {
    scoped_queue s(2);
    size_t wanted = 3 * CHUNK_PAYLOAD;

    EXPECT_EQ(s.q.append(patterned(0, wanted), wanted), 2 * CHUNK_PAYLOAD);
    EXPECT_EQ(s.q.free_space(), 0u);
    EXPECT_EQ(s.q.append(g_src, 1), 0u);

    EXPECT_EQ(s.q.consume(CHUNK_PAYLOAD), CHUNK_PAYLOAD);
    EXPECT_EQ(s.q.chunk_count(), 1u);
    EXPECT_EQ(s.q.free_space(), CHUNK_PAYLOAD);
    EXPECT_EQ(s.q.append(patterned(2 * CHUNK_PAYLOAD, CHUNK_PAYLOAD), CHUNK_PAYLOAD), CHUNK_PAYLOAD);
    EXPECT_EQ(s.q.copy_out(0, g_dst, 2 * CHUNK_PAYLOAD), 2 * CHUNK_PAYLOAD);
    EXPECT_TRUE(matches_pattern(g_dst, CHUNK_PAYLOAD, 2 * CHUNK_PAYLOAD));
}

TEST(tcp_byte_queue, lowering_the_limit_stops_growth_without_touching_stored_bytes) {
    scoped_queue s(4);
    size_t total = 3 * CHUNK_PAYLOAD;
    EXPECT_EQ(s.q.append(patterned(0, total), total), total);

    s.q.set_limit(2);
    EXPECT_EQ(s.q.free_space(), 0u);
    EXPECT_EQ(s.q.append(g_src, 1), 0u);
    EXPECT_EQ(s.q.size(), total);
    EXPECT_EQ(s.q.chunk_count(), 3u);

    EXPECT_EQ(s.q.consume(2 * CHUNK_PAYLOAD), 2 * CHUNK_PAYLOAD);
    EXPECT_EQ(s.q.chunk_count(), 1u);
    EXPECT_EQ(s.q.free_space(), CHUNK_PAYLOAD);
    EXPECT_EQ(s.q.append(patterned(total, 7), 7), 7u);
    EXPECT_EQ(s.q.chunk_count(), 2u);
}

TEST(tcp_byte_queue, a_partly_filled_tail_and_a_partly_consumed_head_share_one_chunk) {
    scoped_queue s(4);

    EXPECT_EQ(s.q.append(patterned(0, 100), 100), 100u);
    EXPECT_EQ(s.q.free_space(), 4 * CHUNK_PAYLOAD - 100);
    EXPECT_EQ(s.q.consume(40), 40u);
    EXPECT_EQ(s.q.size(), 60u);
    EXPECT_EQ(s.q.chunk_count(), 1u);
    EXPECT_EQ(s.q.free_space(), 4 * CHUNK_PAYLOAD - 100);

    EXPECT_EQ(s.q.append(patterned(100, 5), 5), 5u);
    EXPECT_EQ(s.q.chunk_count(), 1u);
    EXPECT_EQ(s.q.copy_out(0, g_dst, 65), 65u);
    EXPECT_TRUE(matches_pattern(g_dst, 40, 65));
}

TEST(tcp_byte_queue, the_global_budget_stops_growth_and_comes_back_on_release) {
    size_t budget_before = budget_in_use();
    __dbg_test_set_budget(budget_before + CHUNK_SIZE);
    {
        scoped_queue s(8);
        EXPECT_EQ(s.q.append(patterned(0, CHUNK_PAYLOAD), CHUNK_PAYLOAD), CHUNK_PAYLOAD);
        EXPECT_EQ(s.q.append(g_src, 1), 0u);
        EXPECT_EQ(s.q.chunk_count(), 1u);
        EXPECT_EQ(budget_in_use(), budget_before + CHUNK_SIZE);
        EXPECT_FALSE(reserve_budget(1));

        s.q.clear();
        EXPECT_EQ(budget_in_use(), budget_before);
        EXPECT_TRUE(reserve_budget(CHUNK_SIZE));
        release_budget(CHUNK_SIZE);
    }

    __dbg_test_set_budget(GLOBAL_BUDGET);
    EXPECT_EQ(budget_in_use(), budget_before);
}

TEST(tcp_byte_queue, a_long_run_of_appends_and_consumes_keeps_every_byte_in_order) {
    size_t budget_before = budget_in_use();
    scoped_queue s(8);
    size_t produced = 0;
    size_t consumed = 0;
    size_t step = 1;

    while (consumed < 1024 * 1024) {
        size_t want = (step * 613) % 3001 + 1;
        size_t took = s.q.append(patterned(produced, want), want);
        produced += took;
        EXPECT_TRUE(s.q.chunk_count() <= 8);

        size_t drain = (step * 419) % 2501 + 1;
        size_t got = s.q.copy_out(0, g_dst, drain);
        EXPECT_TRUE(matches_pattern(g_dst, consumed, got));
        EXPECT_EQ(s.q.consume(got), got);
        consumed += got;
        EXPECT_EQ(s.q.size(), produced - consumed);
        step++;
    }

    EXPECT_EQ(s.q.consume(s.q.size()), produced - consumed);
    EXPECT_EQ(s.q.chunk_count(), 0u);
    EXPECT_EQ(budget_in_use(), budget_before);
}

TEST(tcp_byte_queue, clear_returns_every_chunk) {
    size_t budget_before = budget_in_use();
    scoped_queue s(4);
    EXPECT_EQ(s.q.append(patterned(0, 3 * CHUNK_PAYLOAD), 3 * CHUNK_PAYLOAD), 3 * CHUNK_PAYLOAD);
    EXPECT_EQ(s.q.consume(10), 10u);

    s.q.clear();
    EXPECT_EQ(s.q.size(), 0u);
    EXPECT_EQ(s.q.chunk_count(), 0u);
    EXPECT_EQ(s.q.free_space(), 4 * CHUNK_PAYLOAD);
    EXPECT_EQ(budget_in_use(), budget_before);
    EXPECT_EQ(s.q.append(patterned(0, 5), 5), 5u);
}
