// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "drivers/graphics/nvidia/completion_notifier.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "tests/memory/dma_test_access.h"

TEST_SUITE(nvidia_completion_notifier);

using drivers::nvidia::completion_notifier;
using drivers::nvidia::COMPLETION_NOTIFIER_BYTES;
using drivers::nvidia::ERR_NO_MEMORY;
using drivers::nvidia::OK;

// NVIDIA's size of a display channel's completion notifier, restated so a mistake in the driver's copy is caught
static constexpr uint64_t NVIDIA_NOTIFIER_BYTES = 0x1000;

// A card's full address width, and a limit `dma::alloc_pages` refuses
static constexpr uint8_t FULL_ADDRESS_BITS    = 64;
static constexpr uint8_t REFUSED_ADDRESS_BITS = 16;

// The notifier is NVIDIA's size, with no address before its page is allocated, then a nonzero one aligned to its size,
// meeting a context DMA's 256-byte alignment. The test leaves the page allocated, as the notifier cannot free it
TEST(nvidia_completion_notifier, allocate_gives_an_address_aligned_to_nvidias_size) {
    EXPECT_EQ(COMPLETION_NOTIFIER_BYTES, NVIDIA_NOTIFIER_BYTES);

    completion_notifier notifier;
    EXPECT_EQ(notifier.physical_address(), 0u);

    dma::device_profile card = dma::device_profile_test_access::build(FULL_ADDRESS_BITS, true);
    ASSERT_EQ(notifier.allocate(card), OK);
    EXPECT_NE(notifier.physical_address(), 0u);
    EXPECT_EQ(notifier.physical_address() % NVIDIA_NOTIFIER_BYTES, 0u);
}

// A card whose address limit `dma::alloc_pages` refuses fails the allocation with `ERR_NO_MEMORY`, leaving the notifier
// without an address
TEST(nvidia_completion_notifier, refused_allocation_leaves_no_address) {
    completion_notifier notifier;
    dma::device_profile card = dma::device_profile_test_access::build(REFUSED_ADDRESS_BITS, true);

    EXPECT_EQ(notifier.allocate(card), ERR_NO_MEMORY);
    EXPECT_EQ(notifier.physical_address(), 0u);
}
