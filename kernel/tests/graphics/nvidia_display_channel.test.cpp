// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "clock/clock.h"
#include "drivers/graphics/nvidia/display_channel.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/registers.h"

TEST_SUITE(nvidia_display_channel);

using drivers::nvidia::display_channel;
using drivers::nvidia::ERR_BAD_STATE;
using drivers::nvidia::ERR_INVALID_ARG;
using drivers::nvidia::ERR_QUEUE_FULL;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::METHOD_VALUES_MAX;
using drivers::nvidia::NS_PER_MS;
using drivers::nvidia::OK;
using drivers::nvidia::PUSH_BUFFER_BYTES;
using drivers::nvidia::PUSH_BUFFER_PAD_BYTES;
using drivers::nvidia::registers;

// NVIDIA's push buffer of 4 KiB with its 48-byte pad, its jump to offset 0, the shift of a header's count and the most
// values a method carries in it, restated here so the tests hold the channel to NVIDIA's values rather than to its own
static constexpr uint32_t NVIDIA_PUSH_BUFFER_BYTES = 0x1000;
static constexpr uint32_t NVIDIA_PAD_BYTES         = 48;
static constexpr uint32_t NVIDIA_WRITABLE_END      = NVIDIA_PUSH_BUFFER_BYTES - NVIDIA_PAD_BYTES;
static constexpr uint32_t NVIDIA_JUMP_TO_START     = 0x20000000;
static constexpr uint32_t NVIDIA_COUNT_SHIFT       = 18;
static constexpr uint32_t NVIDIA_METHOD_VALUES_MAX = 1010;

// The push buffer and the channel's area, ordinary memory standing in for DMA pages and BAR0
alignas(4096) static uint32_t g_push_buffer[NVIDIA_PUSH_BUFFER_BYTES / 4];
static uint32_t g_area[2];

// PUT's and GET's words in the area, and a pattern no method writes, so a test sees which words a call wrote
static constexpr uint32_t PUT_WORD      = 0;
static constexpr uint32_t GET_WORD      = 1;
static constexpr uint32_t STALE_PATTERN = 0xA5A5A5A5;

// Values for the methods the tests write, as many as a method carries, the last one distinct from the stale pattern
static constexpr uint32_t VALUES[NVIDIA_METHOD_VALUES_MAX] = {0x11, 0x22, 0x33};

// A register offset for the methods, with the lowest and highest bits a header's offset field holds set, and how long
// the tests' waits last
static constexpr uint32_t METHOD          = 0x2A04;
static constexpr uint64_t CHANNEL_WAIT_NS = NS_PER_MS;

// 1009 values from offset 0 end at 0xFC8, 8 bytes short of the writable end, so no method fits after them without a
// wrap
static constexpr uint32_t FILLING_VALUES = 1009;
static constexpr uint32_t FILLED_END     = 0xFC8;

// A method's count, and a GET short of the room it needs behind GET after a wrap and the first GET leaving that room
struct room_case {
    uint32_t count;
    uint32_t refused_get;
    uint32_t taken_get;
};

// A register offset and count `write` refuses
struct refused_method {
    uint32_t method;
    uint32_t count;
};

// Fills the push buffer with the stale pattern and zeroes PUT and GET, as a channel's allocation leaves them
static void reset_memory() {
    for (uint32_t& word : g_push_buffer) {
        word = STALE_PATTERN;
    }

    g_area[PUT_WORD] = 0;
    g_area[GET_WORD] = 0;
}

// A view of the area's two words, as the driver's view of BAR0 gives the channel
static registers area_view() {
    return registers{reinterpret_cast<uintptr_t>(g_area)};
}

// NVIDIA's header for a method: opcode 0, the count from bit 18 and the register offset in bits 13:2
static uint32_t nvidia_header(uint32_t method, uint32_t count) {
    return count << NVIDIA_COUNT_SHIFT | method;
}

// Writes a method of `count` values and kicks it, then moves GET to PUT, as the display engine reading it would
static void write_and_read(display_channel& channel, uint32_t count) {
    EXPECT_EQ(channel.write(METHOD, VALUES, count), OK);
    channel.kick();
    g_area[GET_WORD] = g_area[PUT_WORD];
}

// Checks that every word of the pad still holds the stale pattern
static void expect_pad_unwritten() {
    for (uint32_t offset = NVIDIA_WRITABLE_END; offset < NVIDIA_PUSH_BUFFER_BYTES; offset += 4) {
        EXPECT_EQ(g_push_buffer[offset / 4], STALE_PATTERN);
    }
}

TEST(nvidia_display_channel, constants_are_nvidias) {
    EXPECT_EQ(PUSH_BUFFER_BYTES, NVIDIA_PUSH_BUFFER_BYTES);
    EXPECT_EQ(PUSH_BUFFER_PAD_BYTES, NVIDIA_PAD_BYTES);
    EXPECT_EQ(METHOD_VALUES_MAX, NVIDIA_METHOD_VALUES_MAX);
}

TEST(nvidia_display_channel, a_method_is_nvidias_header_then_its_values_and_waits_for_a_kick) {
    reset_memory();
    display_channel channel(g_push_buffer, area_view());

    EXPECT_EQ(channel.write(METHOD, VALUES, 2), OK);
    EXPECT_EQ(channel.write(0x3FFC, VALUES + 2, 1), OK);

    EXPECT_EQ(g_push_buffer[0], nvidia_header(METHOD, 2));
    EXPECT_EQ(g_push_buffer[1], 0x11u);
    EXPECT_EQ(g_push_buffer[2], 0x22u);
    EXPECT_EQ(g_push_buffer[3], nvidia_header(0x3FFC, 1));
    EXPECT_EQ(g_push_buffer[4], 0x33u);

    EXPECT_EQ(g_push_buffer[5], STALE_PATTERN);
    EXPECT_EQ(g_area[PUT_WORD], 0u);
}

// 1010 values set the count field's top bit and, from offset 0, end a word short of the pad
TEST(nvidia_display_channel, the_most_values_fit_from_offset_zero_in_one_method) {
    reset_memory();
    display_channel channel(g_push_buffer, area_view());

    EXPECT_EQ(channel.write(METHOD, VALUES, NVIDIA_METHOD_VALUES_MAX), OK);
    channel.kick();

    EXPECT_EQ(g_push_buffer[0], 0x0FC82A04u);
    EXPECT_EQ(g_push_buffer[NVIDIA_METHOD_VALUES_MAX], VALUES[NVIDIA_METHOD_VALUES_MAX - 1]);
    EXPECT_EQ(g_push_buffer[NVIDIA_METHOD_VALUES_MAX + 1], STALE_PATTERN);
    EXPECT_EQ(g_area[PUT_WORD], NVIDIA_WRITABLE_END - 4);
    expect_pad_unwritten();
}

// A fresh channel counts NVIDIA's 1010 free words without reading GET, so a method of 1008 values is written while GET
// reads all ones, and one of 1009 values reads GET first
TEST(nvidia_display_channel, a_fresh_channel_reads_get_only_once_a_method_needs_its_free_words) {
    reset_memory();
    g_area[GET_WORD] = 0xFFFFFFFF;
    display_channel written(g_push_buffer, area_view());
    EXPECT_EQ(written.write(METHOD, VALUES, 1008), OK);
    EXPECT_EQ(g_push_buffer[0], nvidia_header(METHOD, 1008));

    reset_memory();
    g_area[GET_WORD] = 0xFFFFFFFF;
    display_channel refused(g_push_buffer, area_view());
    EXPECT_EQ(refused.write(METHOD, VALUES, 1009), ERR_BAD_STATE);
    EXPECT_EQ(g_push_buffer[0], STALE_PATTERN);
}

TEST(nvidia_display_channel, kick_makes_put_the_written_end_and_skips_an_unmoved_put) {
    reset_memory();
    display_channel channel(g_push_buffer, area_view());

    EXPECT_EQ(channel.write(METHOD, VALUES, 2), OK);
    channel.kick();
    EXPECT_EQ(g_area[PUT_WORD], 12u);

    g_area[PUT_WORD] = STALE_PATTERN;
    channel.kick();
    EXPECT_EQ(g_area[PUT_WORD], STALE_PATTERN);

    EXPECT_EQ(channel.write(METHOD, VALUES, 1), OK);
    channel.kick();
    EXPECT_EQ(g_area[PUT_WORD], 20u);
}

// A method of 10 values takes 44 bytes, so from 0xFA0 it ends at 0xFCC, a word short of the writable end
TEST(nvidia_display_channel, a_method_ending_a_word_short_of_the_writable_end_fits) {
    reset_memory();
    display_channel channel(g_push_buffer, area_view());
    write_and_read(channel, 999);
    EXPECT_EQ(g_area[PUT_WORD], 0xFA0u);

    write_and_read(channel, 10);

    EXPECT_EQ(g_area[PUT_WORD], 0xFCCu);
    EXPECT_EQ(g_push_buffer[0xFA0 / 4], nvidia_header(METHOD, 10));
}

// From 0xFA4 a method of 10 values would end at the writable end, leaving no word for a later jump
TEST(nvidia_display_channel, a_method_reaching_the_writable_end_wraps_with_nvidias_jump) {
    reset_memory();
    display_channel channel(g_push_buffer, area_view());
    write_and_read(channel, 1000);
    EXPECT_EQ(g_area[PUT_WORD], 0xFA4u);

    EXPECT_EQ(channel.write(METHOD, VALUES, 10), OK);

    EXPECT_EQ(g_push_buffer[0xFA4 / 4], NVIDIA_JUMP_TO_START);
    EXPECT_EQ(g_area[PUT_WORD], 0u);
    EXPECT_EQ(g_push_buffer[0], nvidia_header(METHOD, 10));
    expect_pad_unwritten();

    channel.kick();
    EXPECT_EQ(g_area[PUT_WORD], 44u);
}

// The jump goes after a method not yet kicked, so the wrap's kick of PUT to 0 hands the display engine that method too
TEST(nvidia_display_channel, a_wrap_keeps_the_methods_not_yet_kicked_before_its_jump) {
    reset_memory();
    display_channel channel(g_push_buffer, area_view());
    write_and_read(channel, 999);
    EXPECT_EQ(channel.write(METHOD, VALUES, 1), OK);

    EXPECT_EQ(channel.write(METHOD, VALUES, 10), OK);

    EXPECT_EQ(g_push_buffer[0xFA0 / 4], nvidia_header(METHOD, 1));
    EXPECT_EQ(g_push_buffer[0xFA8 / 4], NVIDIA_JUMP_TO_START);
    EXPECT_EQ(g_area[PUT_WORD], 0u);
    EXPECT_EQ(g_push_buffer[0], nvidia_header(METHOD, 10));
}

// A jump to offset 0 while GET is there would make PUT read as GET with the methods before the jump unread. NVIDIA's
// driver kicks the methods written so far and waits, where the channel leaves both to its caller
TEST(nvidia_display_channel, no_wrap_and_no_kick_while_get_is_at_offset_zero) {
    reset_memory();
    display_channel channel(g_push_buffer, area_view());
    EXPECT_EQ(channel.write(METHOD, VALUES, FILLING_VALUES), OK);

    EXPECT_EQ(channel.write(METHOD, VALUES, 1), ERR_QUEUE_FULL);

    EXPECT_EQ(g_push_buffer[FILLED_END / 4], STALE_PATTERN);
    EXPECT_EQ(g_area[PUT_WORD], 0u);
}

// After a wrap the written end is 0. A method of one value (8 bytes) needs GET past the chase pad's 20 bytes, and one
// of 10 values (44 bytes) needs GET past 48 (its end and a word more)
static constexpr room_case ROOM_CASES[] = {{1, 20, 24}, {10, 48, 52}};

// Right after a wrap, with GET still in the previous pass, a method needs GET more than the chase pad past the written
// end and more than a word past its own end. A refused method leaves the wrap's jump written and PUT kicked to 0
TEST(nvidia_display_channel, after_a_wrap_a_method_needs_get_past_nvidias_chase_pad_and_a_word_past_its_end) {
    for (const room_case& room : ROOM_CASES) {
        reset_memory();
        display_channel channel(g_push_buffer, area_view());
        write_and_read(channel, FILLING_VALUES);
        g_area[GET_WORD] = room.refused_get;

        EXPECT_EQ(channel.write(METHOD, VALUES, room.count), ERR_QUEUE_FULL);
        EXPECT_EQ(g_push_buffer[FILLED_END / 4], NVIDIA_JUMP_TO_START);
        EXPECT_EQ(g_area[PUT_WORD], 0u);
        EXPECT_EQ(g_push_buffer[0], nvidia_header(METHOD, FILLING_VALUES));

        g_area[GET_WORD] = room.taken_get;
        EXPECT_EQ(channel.write(METHOD, VALUES, room.count), OK);
        EXPECT_EQ(g_push_buffer[0], nvidia_header(METHOD, room.count));
    }
}

// With GET at 40 after a wrap, the count read there leaves room for four methods of one value, the fourth ending 8
// bytes short of GET, and the fifth finds the written end within the chase pad, leaving the fill's value at 32
TEST(nvidia_display_channel, behind_get_methods_follow_the_free_words_counted_at_the_last_read) {
    reset_memory();
    display_channel channel(g_push_buffer, area_view());
    write_and_read(channel, FILLING_VALUES);
    g_area[GET_WORD] = 40;

    for (uint32_t i = 0; i < 4; i++) {
        EXPECT_EQ(channel.write(METHOD, VALUES, 1), OK);
    }

    EXPECT_EQ(channel.write(METHOD, VALUES, 1), ERR_QUEUE_FULL);
    EXPECT_EQ(g_push_buffer[24 / 4], nvidia_header(METHOD, 1));
    EXPECT_EQ(g_push_buffer[32 / 4], VALUES[7]);
}

// From a written end of 8 a method of 10 values needs GET past 56, and from 52 the chase pad needs GET past 72
TEST(nvidia_display_channel, behind_get_room_is_counted_from_the_written_end) {
    reset_memory();
    display_channel channel(g_push_buffer, area_view());
    write_and_read(channel, FILLING_VALUES);
    g_area[GET_WORD] = 56;
    EXPECT_EQ(channel.write(METHOD, VALUES, 1), OK);

    EXPECT_EQ(channel.write(METHOD, VALUES, 10), ERR_QUEUE_FULL);
    g_area[GET_WORD] = 60;
    EXPECT_EQ(channel.write(METHOD, VALUES, 10), OK);
    EXPECT_EQ(g_push_buffer[8 / 4], nvidia_header(METHOD, 10));

    g_area[GET_WORD] = 72;
    EXPECT_EQ(channel.write(METHOD, VALUES, 1), ERR_QUEUE_FULL);
    g_area[GET_WORD] = 76;
    EXPECT_EQ(channel.write(METHOD, VALUES, 1), OK);
    EXPECT_EQ(g_push_buffer[52 / 4], nvidia_header(METHOD, 1));
}

// After 511 values end at 0x800, a method of 600 values wraps on the write after the wait and finds GET still at the
// jump, so it is written only after another kick and wait, once GET has followed the jump to offset 0
TEST(nvidia_display_channel, a_method_wrapping_after_a_wait_is_written_once_get_has_followed_the_jump) {
    reset_memory();
    display_channel channel(g_push_buffer, area_view());
    EXPECT_EQ(channel.write(METHOD, VALUES, 511), OK);
    EXPECT_EQ(channel.write(METHOD, VALUES, 600), ERR_QUEUE_FULL);

    channel.kick();
    g_area[GET_WORD] = 0x800;
    EXPECT_EQ(channel.wait_idle(clock::now_ns() + CHANNEL_WAIT_NS), OK);
    EXPECT_EQ(channel.write(METHOD, VALUES, 600), ERR_QUEUE_FULL);
    EXPECT_EQ(g_push_buffer[0x800 / 4], NVIDIA_JUMP_TO_START);
    EXPECT_EQ(g_area[PUT_WORD], 0u);

    channel.kick();
    g_area[GET_WORD] = 0;
    EXPECT_EQ(channel.wait_idle(clock::now_ns() + CHANNEL_WAIT_NS), OK);
    EXPECT_EQ(channel.write(METHOD, VALUES, 600), OK);
    EXPECT_EQ(g_push_buffer[0], nvidia_header(METHOD, 600));
}

// Register offsets with bit 0 or bit 1 set, one past the header's bits 13:2 and one whose low 16 bits are valid, then
// no values, a value more than the most, a count whose low 16 bits are valid and one whose byte count wraps to 0
static constexpr refused_method REFUSED_METHODS[] = {
    {0x2A05, 1},
    {0x2A06, 1},
    {0x4000, 1},
    {0x12A04, 1},
    {METHOD, 0},
    {METHOD, NVIDIA_METHOD_VALUES_MAX + 1},
    {METHOD, 0x10001},
    {METHOD, 0xFFFFFFFF},
};

TEST(nvidia_display_channel, a_register_offset_no_header_names_or_a_count_outside_1_to_the_most_is_refused) {
    for (const refused_method& refused : REFUSED_METHODS) {
        reset_memory();
        display_channel channel(g_push_buffer, area_view());

        ASSERT_EQ(channel.write(refused.method, VALUES, refused.count), ERR_INVALID_ARG);
        EXPECT_EQ(g_push_buffer[0], STALE_PATTERN);
    }
}

// All ones, as a card that stops responding reads, offsets past the writable end, at and far past the push buffer's
// end, and offsets with bit 0 or bit 1 set
static constexpr uint32_t REFUSED_GETS[] = {
    0xFFFFFFFF,
    NVIDIA_WRITABLE_END + 4,
    NVIDIA_PUSH_BUFFER_BYTES,
    0x80000000,
    0x101,
    6,
};

// GET is read once a method needs the free words counted, before a wrap and while GET lies ahead after one, where the
// fill's value stays. NVIDIA's driver takes a GET past the writable end ahead of the written end as the writable end
TEST(nvidia_display_channel, a_get_past_the_writable_end_or_off_a_word_is_refused) {
    for (uint32_t refused_get : REFUSED_GETS) {
        reset_memory();
        display_channel channel(g_push_buffer, area_view());
        write_and_read(channel, FILLING_VALUES);
        g_area[GET_WORD] = refused_get;

        EXPECT_EQ(channel.write(METHOD, VALUES, 1), ERR_BAD_STATE);
        EXPECT_EQ(g_push_buffer[FILLED_END / 4], STALE_PATTERN);
        EXPECT_EQ(g_area[PUT_WORD], FILLED_END);

        g_area[GET_WORD] = 56;
        EXPECT_EQ(channel.write(METHOD, VALUES, 1), OK);
        g_area[GET_WORD] = refused_get;

        EXPECT_EQ(channel.write(METHOD, VALUES, 10), ERR_BAD_STATE);
        EXPECT_EQ(g_push_buffer[8 / 4], VALUES[1]);
        EXPECT_FALSE(channel.idle());
        EXPECT_EQ(channel.wait_idle(clock::now_ns() + CHANNEL_WAIT_NS), ERR_BAD_STATE);
    }
}

// An area laid over the push buffer at word 1000 has GET's word at offset 0xFA4, where 1000 values from offset 0 end
// and a wrap from there writes its jump
static constexpr uint32_t OVERLAID_AREA_WORD = 1000;

// With GET at 8 the wrap's jump overwrites GET's word, so the read after the wrap is refused. The next method reads GET
// again and finds the written end within the chase pad of GET, rather than writing on the count from before the wrap
TEST(nvidia_display_channel, a_refused_read_after_a_wrap_leaves_no_word_counted_free_until_get_is_read_again) {
    reset_memory();
    uint32_t* overlaid_area = g_push_buffer + OVERLAID_AREA_WORD;
    overlaid_area[PUT_WORD] = 0;
    overlaid_area[GET_WORD] = 0;

    display_channel channel(g_push_buffer, registers{reinterpret_cast<uintptr_t>(overlaid_area)});
    EXPECT_EQ(channel.write(METHOD, VALUES, 1000), OK);
    channel.kick();
    overlaid_area[GET_WORD] = 8;

    EXPECT_EQ(channel.write(METHOD, VALUES, 10), ERR_BAD_STATE);
    EXPECT_EQ(overlaid_area[GET_WORD], NVIDIA_JUMP_TO_START);
    EXPECT_EQ(overlaid_area[PUT_WORD], 0u);

    overlaid_area[GET_WORD] = 8;
    EXPECT_EQ(channel.write(METHOD, VALUES, 1), ERR_QUEUE_FULL);
    EXPECT_EQ(g_push_buffer[0], nvidia_header(METHOD, 1000));
}

// A GET at the writable end, past a jump at the last word before it, is one the channel takes
TEST(nvidia_display_channel, a_get_at_the_writable_end_is_taken) {
    reset_memory();
    display_channel channel(g_push_buffer, area_view());
    write_and_read(channel, NVIDIA_METHOD_VALUES_MAX);
    g_area[GET_WORD] = 4;
    EXPECT_EQ(channel.write(METHOD, VALUES, 1), ERR_QUEUE_FULL);
    EXPECT_EQ(g_push_buffer[(NVIDIA_WRITABLE_END - 4) / 4], NVIDIA_JUMP_TO_START);

    g_area[GET_WORD] = NVIDIA_WRITABLE_END;
    EXPECT_EQ(channel.write(METHOD, VALUES, 10), OK);

    EXPECT_EQ(g_push_buffer[0], nvidia_header(METHOD, 10));
}

TEST(nvidia_display_channel, idle_reads_whether_get_reached_put) {
    reset_memory();
    display_channel channel(g_push_buffer, area_view());
    EXPECT_TRUE(channel.idle());

    EXPECT_EQ(channel.write(METHOD, VALUES, 1), OK);
    EXPECT_TRUE(channel.idle());

    channel.kick();
    EXPECT_FALSE(channel.idle());

    g_area[GET_WORD] = 8;
    EXPECT_TRUE(channel.idle());
}

// The wait ends at the last PUT kicked, not at a method written after it. A wait whose deadline has passed still reads
// GET once, so a channel already idle is not reported late
TEST(nvidia_display_channel, wait_idle_reads_get_at_least_once_and_times_out_short_of_put) {
    reset_memory();
    display_channel channel(g_push_buffer, area_view());
    EXPECT_EQ(channel.write(METHOD, VALUES, 1), OK);
    channel.kick();
    EXPECT_EQ(channel.write(METHOD, VALUES, 1), OK);

    uint64_t start = clock::now_ns();
    EXPECT_EQ(channel.wait_idle(start + CHANNEL_WAIT_NS), ERR_TIMEOUT);
    EXPECT_GE(clock::now_ns() - start, CHANNEL_WAIT_NS);

    g_area[GET_WORD] = 8;
    EXPECT_EQ(channel.wait_idle(0), OK);
}
