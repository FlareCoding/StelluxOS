// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/vram_allocator.h"

TEST_SUITE(nvidia_vram_allocator);

using drivers::nvidia::ERR_BAD_MESSAGE;
using drivers::nvidia::ERR_INVALID_ARG;
using drivers::nvidia::ERR_NO_MEMORY;
using drivers::nvidia::OK;
using drivers::nvidia::static_configuration;
using drivers::nvidia::video_memory_region;
using drivers::nvidia::vram_allocator;
using drivers::nvidia::vram_range;
using drivers::nvidia::vram_use;

static constexpr uint64_t PAGE_BYTES = 0x1000;

// The offset tests read for an allocation `allocate` refuses, never a multiple of 4 KiB as handed out offsets are
static constexpr uint64_t REFUSED = 1;

// A range no call hands out, filling `out` before a call so the test sees whether the call wrote it
static constexpr vram_range UNTOUCHED = {.offset = 1, .size = 1};

// The RTX 3080's 10 GiB and six regions as its firmware reports them in a boot without a console on the card, the
// usable region 2 and the reserved region 3 holding BAR1's root page directory both of performance 10
static constexpr uint64_t RTX_3080_VIDEO_MEMORY_BYTES = 0x280000000;
static constexpr uint64_t RTX_3080_USABLE_OFFSET      = 0x2A40000;
static constexpr uint64_t RTX_3080_USABLE_BYTES       = 0x2728E0000 - RTX_3080_USABLE_OFFSET;
static constexpr uint64_t RTX_3080_BAR1_ROOT          = 0x2744A9000;

// The RTX 3080's boot console and a scanout buffer for its 2560 by 1440 display both take 1440 rows of 16384 bytes
static constexpr uint64_t RTX_3080_CONSOLE_BYTES = 0x1680000;

// A region the firmware reserves whole, of performance 0, unprotected and supporting neither scanout nor compression
static video_memory_region rtx_3080_reserved_region(uint64_t offset, uint64_t size) {
    return {.offset = offset, .size = size, .reserved_bytes = size, .is_reserved = true};
}

// The RTX 3080's static configuration, its six regions in the firmware's order
static static_configuration rtx_3080_configuration() {
    static_configuration configuration = {};
    configuration.regions[0] = rtx_3080_reserved_region(0, 0x40000);
    configuration.regions[1] = rtx_3080_reserved_region(0x40000, 0x2A00000);
    configuration.regions[2] = {
        .offset = RTX_3080_USABLE_OFFSET,
        .size = RTX_3080_USABLE_BYTES,
        .performance = 10,
        .supports_scanout = true,
        .supports_compression = true,
    };

    configuration.regions[3] = {
        .offset = 0x2728E0000,
        .size = 0x30B0000,
        .reserved_bytes = 0x30B0000,
        .performance = 10,
        .is_reserved = true,
        .supports_scanout = true,
        .supports_compression = true,
    };

    configuration.regions[4] = rtx_3080_reserved_region(0x275990000, 0x70000);
    configuration.regions[5] = rtx_3080_reserved_region(0x275A00000, 0xA600000);
    configuration.region_count = 6;
    configuration.bar1_page_directory_offset = RTX_3080_BAR1_ROOT;
    return configuration;
}

// Most tests' tables: 256 MiB of video memory whose first MiB is a reserved region holding BAR1's root entry, as `init`
// asks, and the regions each test adds
static constexpr uint64_t VIDEO_MEMORY_BYTES = 0x10000000;
static constexpr uint64_t ROOT_REGION_BYTES  = 0x100000;
static constexpr uint64_t ROOT_ENTRY_OFFSET  = 0x1000;
static constexpr uint64_t ROOT_ENTRY_BYTES   = 8;

// The reserved region holding BAR1's root entry
static video_memory_region root_region() {
    return {.offset = 0, .size = ROOT_REGION_BYTES, .reserved_bytes = 0x10000, .is_reserved = true};
}

// Appends `region` to the configuration's regions, counting it
static void add_region(static_configuration& configuration, const video_memory_region& region) {
    configuration.regions[configuration.region_count] = region;
    configuration.region_count++;
}

// A static configuration with BAR1's root entry at `ROOT_ENTRY_OFFSET` and no regions, for a test to list in any order
static static_configuration configuration_without_regions() {
    static_configuration configuration = {};
    configuration.bar1_page_directory_offset = ROOT_ENTRY_OFFSET;
    return configuration;
}

// A static configuration holding only the reserved region with BAR1's root entry
static static_configuration root_region_configuration() {
    static_configuration configuration = configuration_without_regions();
    add_region(configuration, root_region());
    return configuration;
}

// What `init` returns for `configuration` over `VIDEO_MEMORY_BYTES` of video memory, on an allocator of its own
static int32_t init_result(const static_configuration& configuration) {
    vram_allocator allocator;
    return allocator.init(configuration, VIDEO_MEMORY_BYTES);
}

// The offset `allocate` hands out for `size` bytes for `use` at 4 KiB alignment, or `REFUSED`
static uint64_t allocated_offset(vram_allocator& allocator, uint64_t size, vram_use use = vram_use::general) {
    vram_range range = {};
    if (allocator.allocate(size, PAGE_BYTES, use, range) != OK) {
        return REFUSED;
    }

    return range.offset;
}

TEST(nvidia_vram_allocator, rtx_3080_allocations_lie_in_its_usable_region_and_never_in_reserved_region_3) {
    vram_allocator allocator;
    ASSERT_EQ(allocator.init(rtx_3080_configuration(), RTX_3080_VIDEO_MEMORY_BYTES), OK);

    vram_range scanout_buffer = UNTOUCHED;
    ASSERT_EQ(allocator.allocate(RTX_3080_CONSOLE_BYTES, PAGE_BYTES, vram_use::scanout, scanout_buffer), OK);
    EXPECT_EQ(scanout_buffer.offset, RTX_3080_USABLE_OFFSET);
    EXPECT_EQ(scanout_buffer.size, RTX_3080_CONSOLE_BYTES);

    uint64_t rest_of_usable_region = RTX_3080_USABLE_BYTES - RTX_3080_CONSOLE_BYTES;
    EXPECT_EQ(allocated_offset(allocator, rest_of_usable_region), RTX_3080_USABLE_OFFSET + RTX_3080_CONSOLE_BYTES);
    EXPECT_EQ(allocated_offset(allocator, PAGE_BYTES, vram_use::scanout), REFUSED);
    EXPECT_EQ(allocated_offset(allocator, PAGE_BYTES), REFUSED);
}

TEST(nvidia_vram_allocator, regions_are_tried_highest_performance_first) {
    static_configuration configuration = root_region_configuration();
    add_region(configuration, {.offset = 0x100000, .size = 0x10000, .performance = 1});
    add_region(configuration, {.offset = 0x200000, .size = 0x10000, .performance = 3});
    add_region(configuration, {.offset = 0x300000, .size = 0x10000, .performance = 2});

    vram_allocator allocator;
    ASSERT_EQ(allocator.init(configuration, VIDEO_MEMORY_BYTES), OK);

    EXPECT_EQ(allocated_offset(allocator, 0x10000), 0x200000u);
    EXPECT_EQ(allocated_offset(allocator, 0x10000), 0x300000u);
    EXPECT_EQ(allocated_offset(allocator, 0x10000), 0x100000u);
    EXPECT_EQ(allocated_offset(allocator, PAGE_BYTES), REFUSED);
}

// Regions of equal performance are tried in the firmware's order, while NVIDIA's exchange sort and a tiebreak by offset
// would both try this table's regions at 0x300000, 0x100000 and 0x200000 in turn
TEST(nvidia_vram_allocator, regions_of_equal_performance_are_tried_in_the_firmwares_order) {
    static_configuration configuration = root_region_configuration();
    add_region(configuration, {.offset = 0x200000, .size = 0x10000, .performance = 1});
    add_region(configuration, {.offset = 0x100000, .size = 0x10000, .performance = 1});
    add_region(configuration, {.offset = 0x300000, .size = 0x10000, .performance = 2});

    vram_allocator allocator;
    ASSERT_EQ(allocator.init(configuration, VIDEO_MEMORY_BYTES), OK);

    EXPECT_EQ(allocated_offset(allocator, 0x10000), 0x300000u);
    EXPECT_EQ(allocated_offset(allocator, 0x10000), 0x200000u);
    EXPECT_EQ(allocated_offset(allocator, 0x10000), 0x100000u);
}

TEST(nvidia_vram_allocator, performance_ranks_as_an_unsigned_32_bit_number) {
    static_configuration configuration = root_region_configuration();
    add_region(configuration, {.offset = 0x100000, .size = 0x10000, .performance = 0xFFFF});
    add_region(configuration, {.offset = 0x200000, .size = 0x10000, .performance = 0x10000});
    add_region(configuration, {.offset = 0x300000, .size = 0x10000, .performance = 0x7FFFFFFF});
    add_region(configuration, {.offset = 0x400000, .size = 0x10000, .performance = 0x80000000});

    vram_allocator allocator;
    ASSERT_EQ(allocator.init(configuration, VIDEO_MEMORY_BYTES), OK);

    EXPECT_EQ(allocated_offset(allocator, 0x10000), 0x400000u);
    EXPECT_EQ(allocated_offset(allocator, 0x10000), 0x300000u);
    EXPECT_EQ(allocated_offset(allocator, 0x10000), 0x200000u);
    EXPECT_EQ(allocated_offset(allocator, 0x10000), 0x100000u);
}

TEST(nvidia_vram_allocator, protected_and_reserved_regions_are_never_used) {
    static_configuration configuration = root_region_configuration();
    add_region(configuration, {.offset = 0x100000, .size = 0x100000, .performance = 5, .is_protected = true});
    add_region(configuration, {.offset = 0x200000, .size = 0x100000, .performance = 5, .is_reserved = true});
    add_region(configuration, {.offset = 0x300000, .size = 0x10000, .performance = 1});

    vram_allocator allocator;
    ASSERT_EQ(allocator.init(configuration, VIDEO_MEMORY_BYTES), OK);

    EXPECT_EQ(allocated_offset(allocator, 0x10000), 0x300000u);
    EXPECT_EQ(allocated_offset(allocator, PAGE_BYTES), REFUSED);

    vram_allocator reserved_only;
    ASSERT_EQ(reserved_only.init(root_region_configuration(), VIDEO_MEMORY_BYTES), OK);
    EXPECT_EQ(allocated_offset(reserved_only, PAGE_BYTES), REFUSED);
}

TEST(nvidia_vram_allocator, scanout_skips_regions_without_scanout_support_that_general_allocations_use) {
    static_configuration configuration = root_region_configuration();
    add_region(configuration, {.offset = 0x100000, .size = 0x10000, .performance = 2});
    add_region(configuration, {.offset = 0x200000, .size = 0x10000, .performance = 1, .supports_scanout = true});

    vram_allocator allocator;
    ASSERT_EQ(allocator.init(configuration, VIDEO_MEMORY_BYTES), OK);

    EXPECT_EQ(allocated_offset(allocator, 0x10000, vram_use::scanout), 0x200000u);
    EXPECT_EQ(allocated_offset(allocator, 0x10000, vram_use::scanout), REFUSED);
    EXPECT_EQ(allocated_offset(allocator, 0x10000, vram_use::general), 0x100000u);
}

TEST(nvidia_vram_allocator, an_allocation_never_spans_two_regions_or_uses_one_past_the_count) {
    static_configuration configuration = root_region_configuration();
    add_region(configuration, {.offset = 0x100000, .size = PAGE_BYTES});
    add_region(configuration, {.offset = 0x101000, .size = PAGE_BYTES});
    configuration.regions[3] = {.offset = 0x200000, .size = 0x100000};

    vram_allocator allocator;
    ASSERT_EQ(allocator.init(configuration, VIDEO_MEMORY_BYTES), OK);

    EXPECT_EQ(allocated_offset(allocator, 2 * PAGE_BYTES), REFUSED);
    EXPECT_EQ(allocated_offset(allocator, PAGE_BYTES), 0x100000u);
    EXPECT_EQ(allocated_offset(allocator, PAGE_BYTES), 0x101000u);
    EXPECT_EQ(allocated_offset(allocator, PAGE_BYTES), REFUSED);
}

TEST(nvidia_vram_allocator, a_region_starting_off_a_4_kib_boundary_hands_out_ranges_from_the_next_one) {
    static_configuration configuration = root_region_configuration();
    add_region(configuration, {.offset = 0x1900100, .size = 2 * PAGE_BYTES - 0x100});

    vram_allocator allocator;
    ASSERT_EQ(allocator.init(configuration, VIDEO_MEMORY_BYTES), OK);

    vram_range byte_aligned = UNTOUCHED;
    ASSERT_EQ(allocator.allocate(PAGE_BYTES, 1, vram_use::general, byte_aligned), OK);
    EXPECT_EQ(byte_aligned.offset, 0x1901000u);
    EXPECT_EQ(allocated_offset(allocator, PAGE_BYTES), REFUSED);
}

TEST(nvidia_vram_allocator, ranges_lie_at_the_lowest_free_offset_meeting_the_alignment) {
    static_configuration configuration = root_region_configuration();
    add_region(configuration, {.offset = 0x100000, .size = 0x400000});

    vram_allocator allocator;
    ASSERT_EQ(allocator.init(configuration, VIDEO_MEMORY_BYTES), OK);

    vram_range first = UNTOUCHED;
    vram_range large = UNTOUCHED;
    vram_range below_large = UNTOUCHED;
    vram_range small = UNTOUCHED;
    ASSERT_EQ(allocator.allocate(1, 1, vram_use::general, first), OK);
    ASSERT_EQ(allocator.allocate(PAGE_BYTES, 0x200000, vram_use::general, large), OK);
    ASSERT_EQ(allocator.allocate(PAGE_BYTES + 1, PAGE_BYTES, vram_use::general, below_large), OK);
    ASSERT_EQ(allocator.allocate(PAGE_BYTES, 0x800, vram_use::general, small), OK);

    EXPECT_EQ(first.offset, 0x100000u);
    EXPECT_EQ(first.size, PAGE_BYTES);
    EXPECT_EQ(large.offset, 0x200000u);
    EXPECT_EQ(below_large.offset, 0x101000u);
    EXPECT_EQ(below_large.size, 2 * PAGE_BYTES);
    EXPECT_EQ(small.offset, 0x103000u);
}

TEST(nvidia_vram_allocator, a_released_range_is_handed_out_again) {
    static_configuration configuration = root_region_configuration();
    add_region(configuration, {.offset = 0x100000, .size = 3 * PAGE_BYTES});

    vram_allocator allocator;
    ASSERT_EQ(allocator.init(configuration, VIDEO_MEMORY_BYTES), OK);

    vram_range middle = UNTOUCHED;
    EXPECT_EQ(allocated_offset(allocator, PAGE_BYTES), 0x100000u);
    ASSERT_EQ(allocator.allocate(PAGE_BYTES, PAGE_BYTES, vram_use::general, middle), OK);
    EXPECT_EQ(allocated_offset(allocator, PAGE_BYTES), 0x102000u);
    EXPECT_EQ(allocated_offset(allocator, PAGE_BYTES), REFUSED);

    allocator.release(middle);
    EXPECT_EQ(allocated_offset(allocator, PAGE_BYTES), 0x101000u);
}

TEST(nvidia_vram_allocator, a_range_is_handed_out_only_whole_and_until_it_is_released) {
    static_configuration configuration = root_region_configuration();
    add_region(configuration, {.offset = 0x100000, .size = 2 * PAGE_BYTES});

    vram_allocator allocator;
    ASSERT_EQ(allocator.init(configuration, VIDEO_MEMORY_BYTES), OK);

    vram_range handed_out = UNTOUCHED;
    ASSERT_EQ(allocator.allocate(2 * PAGE_BYTES, PAGE_BYTES, vram_use::general, handed_out), OK);
    EXPECT_TRUE(allocator.is_handed_out(handed_out));
    EXPECT_FALSE(allocator.is_handed_out({.offset = handed_out.offset, .size = PAGE_BYTES}));
    EXPECT_FALSE(allocator.is_handed_out({.offset = handed_out.offset + PAGE_BYTES, .size = PAGE_BYTES}));

    allocator.release(handed_out);
    EXPECT_FALSE(allocator.is_handed_out(handed_out));
}

TEST(nvidia_vram_allocator, releasing_a_range_matching_no_allocation_changes_nothing) {
    static_configuration configuration = root_region_configuration();
    add_region(configuration, {.offset = 0x100000, .size = 2 * PAGE_BYTES});

    vram_allocator allocator;
    ASSERT_EQ(allocator.init(configuration, VIDEO_MEMORY_BYTES), OK);

    vram_range held = UNTOUCHED;
    ASSERT_EQ(allocator.allocate(PAGE_BYTES, PAGE_BYTES, vram_use::general, held), OK);
    allocator.release({.offset = held.offset, .size = 2 * PAGE_BYTES});
    allocator.release({.offset = 0x101000, .size = PAGE_BYTES});
    EXPECT_EQ(allocated_offset(allocator, PAGE_BYTES), 0x101000u);
    EXPECT_EQ(allocated_offset(allocator, PAGE_BYTES), REFUSED);

    allocator.release(held);
    allocator.release(held);
    EXPECT_EQ(allocated_offset(allocator, PAGE_BYTES), 0x100000u);
    EXPECT_EQ(allocated_offset(allocator, PAGE_BYTES), REFUSED);
}

TEST(nvidia_vram_allocator, refused_calls_leave_out_unchanged) {
    static_configuration configuration = root_region_configuration();
    add_region(configuration, {.offset = 0x100000, .size = 0x100000});

    vram_allocator allocator;
    ASSERT_EQ(allocator.init(configuration, VIDEO_MEMORY_BYTES), OK);

    vram_range out = UNTOUCHED;
    EXPECT_EQ(allocator.allocate(0, PAGE_BYTES, vram_use::general, out), ERR_INVALID_ARG);
    EXPECT_EQ(allocator.allocate(0 - PAGE_BYTES + 1, PAGE_BYTES, vram_use::general, out), ERR_INVALID_ARG);
    EXPECT_EQ(allocator.allocate(0 - PAGE_BYTES, PAGE_BYTES, vram_use::general, out), ERR_NO_MEMORY);
    EXPECT_EQ(allocator.allocate(PAGE_BYTES, 0, vram_use::general, out), ERR_INVALID_ARG);
    EXPECT_EQ(allocator.allocate(PAGE_BYTES, 3, vram_use::general, out), ERR_INVALID_ARG);
    EXPECT_EQ(allocator.allocate(PAGE_BYTES, 3 * PAGE_BYTES, vram_use::general, out), ERR_INVALID_ARG);
    EXPECT_EQ(allocator.allocate(0x200000, PAGE_BYTES, vram_use::general, out), ERR_NO_MEMORY);
    EXPECT_EQ(allocator.allocate(PAGE_BYTES, 0x200000, vram_use::general, out), ERR_NO_MEMORY);
    EXPECT_EQ(allocator.allocate(PAGE_BYTES, PAGE_BYTES, vram_use::scanout, out), ERR_NO_MEMORY);
    EXPECT_EQ(out.offset, UNTOUCHED.offset);
    EXPECT_EQ(out.size, UNTOUCHED.size);
}

TEST(nvidia_vram_allocator, nothing_is_handed_out_before_init_succeeds) {
    vram_allocator fresh;
    vram_range out = UNTOUCHED;
    EXPECT_EQ(fresh.allocate(PAGE_BYTES, PAGE_BYTES, vram_use::general, out), ERR_NO_MEMORY);

    static_configuration configuration = root_region_configuration();
    add_region(configuration, {.offset = 0x100000, .size = 0x100000});
    configuration.bar1_page_directory_offset = 0x100000;

    vram_allocator refused;
    EXPECT_EQ(refused.init(configuration, VIDEO_MEMORY_BYTES), ERR_BAD_MESSAGE);
    EXPECT_EQ(refused.allocate(PAGE_BYTES, PAGE_BYTES, vram_use::general, out), ERR_NO_MEMORY);
    EXPECT_EQ(out.offset, UNTOUCHED.offset);
    EXPECT_EQ(out.size, UNTOUCHED.size);
}

// Video memory of 2^63 bytes and 8 KiB, holding regions whose candidate offsets round up past 2^64 - 1
static constexpr uint64_t PAST_2_63_BYTES = 0x8000000000002000;

TEST(nvidia_vram_allocator, an_offset_rounding_up_past_2_64_is_not_handed_out) {
    static_configuration region_start_rounds_past = root_region_configuration();
    add_region(region_start_rounds_past, {.offset = 0x8000000000001000, .size = PAGE_BYTES});

    static_configuration record_end_rounds_past = root_region_configuration();
    add_region(record_end_rounds_past, {.offset = 0x8000000000000000, .size = 2 * PAGE_BYTES});

    vram_allocator allocator_rounding_region_start;
    ASSERT_EQ(allocator_rounding_region_start.init(region_start_rounds_past, PAST_2_63_BYTES), OK);

    vram_allocator allocator_rounding_record_end;
    ASSERT_EQ(allocator_rounding_record_end.init(record_end_rounds_past, PAST_2_63_BYTES), OK);
    ASSERT_EQ(allocated_offset(allocator_rounding_record_end, PAGE_BYTES), 0x8000000000000000u);

    vram_range out = UNTOUCHED;
    EXPECT_EQ(allocator_rounding_region_start.allocate(PAGE_BYTES, 1ULL << 63, vram_use::general, out), ERR_NO_MEMORY);
    EXPECT_EQ(allocator_rounding_record_end.allocate(PAGE_BYTES, 1ULL << 63, vram_use::general, out), ERR_NO_MEMORY);
    EXPECT_EQ(out.offset, UNTOUCHED.offset);
}

TEST(nvidia_vram_allocator, a_second_init_replaces_the_regions_unless_refused) {
    static_configuration first = root_region_configuration();
    add_region(first, {.offset = 0x100000, .size = PAGE_BYTES});

    static_configuration second = root_region_configuration();
    add_region(second, {.offset = 0x200000, .size = PAGE_BYTES});

    static_configuration refused = root_region_configuration();
    add_region(refused, {.offset = 0x300000, .size = PAGE_BYTES});
    refused.bar1_page_directory_offset = 0x300000;

    vram_allocator allocator;
    ASSERT_EQ(allocator.init(first, VIDEO_MEMORY_BYTES), OK);
    ASSERT_EQ(allocator.init(second, VIDEO_MEMORY_BYTES), OK);
    ASSERT_EQ(allocator.init(refused, VIDEO_MEMORY_BYTES), ERR_BAD_MESSAGE);

    EXPECT_EQ(allocated_offset(allocator, PAGE_BYTES), 0x200000u);
    EXPECT_EQ(allocated_offset(allocator, PAGE_BYTES), REFUSED);
}

TEST(nvidia_vram_allocator, overlapping_regions_are_refused_and_adjacent_ones_taken) {
    static_configuration overlapping = root_region_configuration();
    add_region(overlapping, {.offset = ROOT_REGION_BYTES - 1, .size = 0x100000});

    static_configuration overlapping_listed_higher_offset_first = configuration_without_regions();
    add_region(overlapping_listed_higher_offset_first, {.offset = ROOT_REGION_BYTES - 1, .size = 0x100000});
    add_region(overlapping_listed_higher_offset_first, root_region());

    static_configuration overlapping_reserved = root_region_configuration();
    add_region(overlapping_reserved, {.offset = 0x200000, .size = 0x100000, .is_reserved = true});
    add_region(overlapping_reserved, {.offset = 0x2FF000, .size = 0x100000, .is_reserved = true});

    static_configuration overlapping_apart_in_the_table = root_region_configuration();
    add_region(overlapping_apart_in_the_table, {.offset = 0x200000, .size = 0x100000});
    add_region(overlapping_apart_in_the_table, {.offset = 0x400000, .size = 0x100000});
    add_region(overlapping_apart_in_the_table, {.offset = 0x2FF000, .size = 2 * PAGE_BYTES});

    static_configuration overlapping_above_4_gib = root_region_configuration();
    add_region(overlapping_above_4_gib, {.offset = 0x100000000, .size = 0x100000, .is_reserved = true});
    add_region(overlapping_above_4_gib, {.offset = 0x1000FF000, .size = 0x100000, .is_reserved = true});

    static_configuration adjacent = root_region_configuration();
    add_region(adjacent, {.offset = ROOT_REGION_BYTES, .size = 0x100000});

    static_configuration adjacent_listed_highest_offset_first = configuration_without_regions();
    add_region(adjacent_listed_highest_offset_first, {.offset = 0x200000, .size = 0x100000});
    add_region(adjacent_listed_highest_offset_first, {.offset = ROOT_REGION_BYTES, .size = 0x100000});
    add_region(adjacent_listed_highest_offset_first, root_region());

    static_configuration overlapping_past_the_count = root_region_configuration();
    overlapping_past_the_count.regions[1] = {.offset = ROOT_REGION_BYTES - PAGE_BYTES, .size = 0x100000};

    EXPECT_EQ(init_result(overlapping), ERR_BAD_MESSAGE);
    EXPECT_EQ(init_result(overlapping_listed_higher_offset_first), ERR_BAD_MESSAGE);
    EXPECT_EQ(init_result(overlapping_reserved), ERR_BAD_MESSAGE);
    EXPECT_EQ(init_result(overlapping_apart_in_the_table), ERR_BAD_MESSAGE);
    EXPECT_EQ(init_result(overlapping_above_4_gib), ERR_BAD_MESSAGE);
    EXPECT_EQ(init_result(adjacent), OK);
    EXPECT_EQ(init_result(adjacent_listed_highest_offset_first), OK);
    EXPECT_EQ(init_result(overlapping_past_the_count), OK);
}

TEST(nvidia_vram_allocator, usable_regions_ending_past_video_memory_or_at_2_64_are_refused) {
    static_configuration ending_at_size = root_region_configuration();
    add_region(ending_at_size, {.offset = VIDEO_MEMORY_BYTES - 0x100000, .size = 0x100000});

    static_configuration ending_past_size = root_region_configuration();
    add_region(ending_past_size, {.offset = VIDEO_MEMORY_BYTES - 0x100000, .size = 0x100000 + PAGE_BYTES});

    static_configuration past_size_before_a_valid_region = root_region_configuration();
    add_region(past_size_before_a_valid_region, {.offset = VIDEO_MEMORY_BYTES, .size = 0x100000});
    add_region(past_size_before_a_valid_region, {.offset = 0x100000, .size = 0x100000});

    static_configuration past_size_first = configuration_without_regions();
    add_region(past_size_first, {.offset = VIDEO_MEMORY_BYTES, .size = 0x100000});
    add_region(past_size_first, root_region());

    static_configuration protected_past_size = root_region_configuration();
    add_region(protected_past_size, {.offset = VIDEO_MEMORY_BYTES, .size = 0x100000, .is_protected = true});

    static_configuration ending_at_2_64 = root_region_configuration();
    add_region(ending_at_2_64, {.offset = 0xFFFFFFFFFFFFF001, .size = 0xFFF});

    static_configuration reserved_past_size = root_region_configuration();
    add_region(reserved_past_size, {.offset = VIDEO_MEMORY_BYTES, .size = 0x100000, .is_reserved = true});

    static_configuration past_size_past_the_count = root_region_configuration();
    past_size_past_the_count.regions[1] = {.offset = VIDEO_MEMORY_BYTES, .size = 0x100000};

    EXPECT_EQ(init_result(ending_at_size), OK);
    EXPECT_EQ(init_result(ending_past_size), ERR_BAD_MESSAGE);
    EXPECT_EQ(init_result(past_size_before_a_valid_region), ERR_BAD_MESSAGE);
    EXPECT_EQ(init_result(past_size_first), ERR_BAD_MESSAGE);
    EXPECT_EQ(init_result(protected_past_size), ERR_BAD_MESSAGE);
    EXPECT_EQ(init_result(ending_at_2_64), ERR_BAD_MESSAGE);
    EXPECT_EQ(init_result(reserved_past_size), OK);
    EXPECT_EQ(init_result(past_size_past_the_count), OK);
}

TEST(nvidia_vram_allocator, bar1s_root_entry_must_lie_wholly_inside_a_reserved_region) {
    static_configuration last_entry_inside = root_region_configuration();
    last_entry_inside.bar1_page_directory_offset = ROOT_REGION_BYTES - ROOT_ENTRY_BYTES;

    static_configuration straddling_the_end = root_region_configuration();
    straddling_the_end.bar1_page_directory_offset = ROOT_REGION_BYTES - ROOT_ENTRY_BYTES / 2;
    add_region(straddling_the_end, {.offset = ROOT_REGION_BYTES, .size = 0x100000, .is_reserved = true});

    static_configuration in_a_usable_region = root_region_configuration();
    in_a_usable_region.bar1_page_directory_offset = 0x101000;
    add_region(in_a_usable_region, {.offset = 0x100000, .size = 0x100000});

    static_configuration in_no_region = root_region_configuration();
    in_no_region.bar1_page_directory_offset = 0x300000;

    static_configuration in_the_last_region = root_region_configuration();
    in_the_last_region.bar1_page_directory_offset = 0x300000;
    add_region(in_the_last_region, {.offset = 0x100000, .size = 0x100000});
    add_region(in_the_last_region, {.offset = 0x300000, .size = 0x100000, .is_reserved = true});

    static_configuration in_a_region_past_the_count = root_region_configuration();
    in_a_region_past_the_count.bar1_page_directory_offset = 0x300000;
    add_region(in_a_region_past_the_count, {.offset = 0x100000, .size = 0x100000});
    in_a_region_past_the_count.regions[2] = {.offset = 0x300000, .size = 0x100000, .is_reserved = true};

    EXPECT_EQ(init_result(last_entry_inside), OK);
    EXPECT_EQ(init_result(straddling_the_end), ERR_BAD_MESSAGE);
    EXPECT_EQ(init_result(in_a_usable_region), ERR_BAD_MESSAGE);
    EXPECT_EQ(init_result(in_no_region), ERR_BAD_MESSAGE);
    EXPECT_EQ(init_result(in_the_last_region), OK);
    EXPECT_EQ(init_result(in_a_region_past_the_count), ERR_BAD_MESSAGE);
}
