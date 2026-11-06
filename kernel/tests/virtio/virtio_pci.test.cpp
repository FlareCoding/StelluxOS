#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "drivers/virtio/virtio_pci.h"

using drivers::virtio::decode_shared_memory_cap;
using drivers::virtio::shared_memory_fits_bar;
using drivers::virtio::virtio_pci_cap64;

TEST_SUITE(virtio_shared_memory_test);

constexpr uint64_t BAR_PHYS = 0x800000000ULL;
constexpr uint64_t BAR_SIZE = 0x10000000ULL;
constexpr uint64_t SMALL_REGION = 0x1000;

static pci::bar memory_bar() {
    return {BAR_PHYS, BAR_SIZE, pci::BAR_MMIO64, true};
}

static virtio_pci_cap64 shared_memory_cap(uint8_t cap_len) {
    virtio_pci_cap64 cap = {};
    cap.cap.cap_len = cap_len;
    cap.cap.offset = 0x1000;
    cap.cap.length = 0x2000;
    cap.offset_hi = 0x3;
    cap.length_hi = 0x4;

    return cap;
}

TEST(virtio_shared_memory_test, capability_joins_the_upper_words_of_its_offset_and_length) {
    uint64_t offset = 0;
    uint64_t length = 0;

    ASSERT_TRUE(decode_shared_memory_cap(shared_memory_cap(sizeof(virtio_pci_cap64)), &offset, &length));
    EXPECT_EQ(offset, 0x300001000ULL);
    EXPECT_EQ(length, 0x400002000ULL);
}

TEST(virtio_shared_memory_test, capability_too_short_for_its_upper_words_lists_no_region) {
    uint64_t offset = 0;
    uint64_t length = 0;

    EXPECT_FALSE(decode_shared_memory_cap(shared_memory_cap(sizeof(virtio_pci_cap64) - 1), &offset, &length));
}

TEST(virtio_shared_memory_test, region_inside_a_memory_bar_fits) {
    pci::bar bar_32 = memory_bar();
    bar_32.type = pci::BAR_MMIO32;

    EXPECT_TRUE(shared_memory_fits_bar(memory_bar(), 0, BAR_SIZE));
    EXPECT_TRUE(shared_memory_fits_bar(memory_bar(), BAR_SIZE - SMALL_REGION, SMALL_REGION));
    EXPECT_TRUE(shared_memory_fits_bar(memory_bar(), BAR_SIZE, 0));
    EXPECT_TRUE(shared_memory_fits_bar(bar_32, 0, BAR_SIZE));
}

TEST(virtio_shared_memory_test, region_ending_past_its_bar_does_not_fit) {
    EXPECT_FALSE(shared_memory_fits_bar(memory_bar(), SMALL_REGION, BAR_SIZE));
}

TEST(virtio_shared_memory_test, region_starting_past_its_bar_does_not_fit) {
    EXPECT_FALSE(shared_memory_fits_bar(memory_bar(), BAR_SIZE + SMALL_REGION, 0));
}

TEST(virtio_shared_memory_test, region_whose_end_wraps_does_not_fit) {
    EXPECT_FALSE(shared_memory_fits_bar(memory_bar(), SMALL_REGION, ~0ULL - SMALL_REGION / 2));
}

TEST(virtio_shared_memory_test, unassigned_bar_holds_no_region) {
    pci::bar unassigned = memory_bar();
    unassigned.phys = 0;

    EXPECT_FALSE(shared_memory_fits_bar(unassigned, 0, SMALL_REGION));
}

TEST(virtio_shared_memory_test, io_bar_holds_no_region) {
    pci::bar io = memory_bar();
    io.type = pci::BAR_IO;

    EXPECT_FALSE(shared_memory_fits_bar(io, 0, SMALL_REGION));
}
