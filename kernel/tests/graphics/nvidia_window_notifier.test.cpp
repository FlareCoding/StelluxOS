// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "common/logging.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/latch_rules.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/window_notifier.h"
#include "hw/mmio.h"
#include "tests/memory/dma_test_access.h"

TEST_SUITE(nvidia_window_notifier);

using drivers::nvidia::ERR_NO_MEMORY;
using drivers::nvidia::NOTIFIER_SLOTS;
using drivers::nvidia::OK;
using drivers::nvidia::window_notifier;
using drivers::nvidia::WINDOW_NOTIFIER_BYTES;
using drivers::nvidia::WINDOW_NOTIFIER_SLOT_BYTES;

namespace drivers::nvidia {

// Writes and reads a slot's first word as the display engine would
struct window_notifier_test_access {
    static void write_word(window_notifier& notifier, uint32_t slot, uint32_t word) {
        mmio::write32(notifier.m_page.virt + slot * WINDOW_NOTIFIER_SLOT_BYTES, word);
    }

    static uint32_t read_word(const window_notifier& notifier, uint32_t slot) {
        return mmio::read32(notifier.m_page.virt + slot * WINDOW_NOTIFIER_SLOT_BYTES);
    }
};

} // namespace drivers::nvidia

using drivers::nvidia::window_notifier_test_access;

// The bytes of NVIDIA's notifier page and of each slot in it, and a slot's begun and finished statuses in bits 31:30 of
// its first word, restated so a mistake in the driver's copy is caught
static constexpr uint64_t NVIDIA_NOTIFIER_BYTES = 0x1000;
static constexpr uint32_t NVIDIA_SLOT_BYTES     = 16;
static constexpr uint32_t NVIDIA_BEGUN          = 0x40000000;
static constexpr uint32_t NVIDIA_FINISHED       = 0x80000000;

// A first word with 3 in bits 31:30, a status neither begun nor finished
static constexpr uint32_t STATUS_3 = 0xC0000000;

// A card's full address width, and a limit `dma::alloc_pages` refuses
static constexpr uint8_t FULL_ADDRESS_BITS    = 64;
static constexpr uint8_t REFUSED_ADDRESS_BITS = 16;

// The text logged while `NOTIFIER_LOG_BACKEND` is set
static char     s_notifier_log[512] = {};
static uint32_t s_notifier_log_bytes = 0;

// Keeps each logged line's text, up to the buffer's end
static void keep_notifier_log(const char* data, size_t len) {
    for (size_t i = 0; i < len && s_notifier_log_bytes < sizeof(s_notifier_log); i++) {
        s_notifier_log[s_notifier_log_bytes++] = data[i];
    }
}

static constexpr log::backend NOTIFIER_LOG_BACKEND = {.write = keep_notifier_log};

// Whether the kept text holds `text` anywhere
static bool notifier_log_contains(const char* text) {
    size_t text_bytes = string::strlen(text);
    for (uint32_t start = 0; start + text_bytes <= s_notifier_log_bytes; start++) {
        if (string::memcmp(s_notifier_log + start, text, text_bytes) == 0) {
            return true;
        }
    }

    return false;
}

// Allocates the notifier's page for `card` with the log kept
static int32_t allocate_logged(window_notifier& notifier, const dma::device_profile& card) {
    s_notifier_log_bytes = 0;
    log::set_backend(&NOTIFIER_LOG_BACKEND);
    int32_t rc = notifier.allocate(card);
    log::set_backend(nullptr);

    return rc;
}

// The page and its slots are NVIDIA's sizes, with no address before the page is allocated, then one aligned to its
// size, meeting a context DMA's 256-byte alignment. The test leaves the page allocated, as the notifier cannot free it
TEST(nvidia_window_notifier, allocate_gives_an_address_aligned_to_nvidias_size) {
    EXPECT_EQ(WINDOW_NOTIFIER_BYTES, NVIDIA_NOTIFIER_BYTES);
    EXPECT_EQ(WINDOW_NOTIFIER_SLOT_BYTES, NVIDIA_SLOT_BYTES);

    window_notifier notifier;
    EXPECT_EQ(notifier.physical_address(), 0u);

    dma::device_profile card = dma::device_profile_test_access::build(FULL_ADDRESS_BITS, true);
    ASSERT_EQ(notifier.allocate(card), OK);
    EXPECT_NE(notifier.physical_address(), 0u);
    EXPECT_EQ(notifier.physical_address() % NVIDIA_NOTIFIER_BYTES, 0u);
}

// A card whose address limit `dma::alloc_pages` refuses fails the allocation with `ERR_NO_MEMORY`, logging the
// refusal's error, and leaves no address
TEST(nvidia_window_notifier, refused_allocation_leaves_no_address) {
    window_notifier notifier;
    dma::device_profile card = dma::device_profile_test_access::build(REFUSED_ADDRESS_BITS, true);

    EXPECT_EQ(allocate_logged(notifier, card), ERR_NO_MEMORY);
    EXPECT_TRUE(notifier_log_contains("nvidia: allocating the window notifier failed: -1\r\n"));
    EXPECT_EQ(notifier.physical_address(), 0u);
}

// A slot reads written once begun or finished, whatever its other bits, and not once cleared, not begun with its other
// bits set, or at status 3, while a clear writes the not-begun status into its own slot alone
TEST(nvidia_window_notifier, a_slot_reads_written_only_once_begun_or_finished) {
    window_notifier notifier;
    dma::device_profile card = dma::device_profile_test_access::build(FULL_ADDRESS_BITS, true);
    ASSERT_EQ(notifier.allocate(card), OK);
    for (uint32_t slot = 0; slot < NOTIFIER_SLOTS; slot++) {
        window_notifier_test_access::write_word(notifier, slot, NVIDIA_FINISHED);
    }

    notifier.clear(1);
    EXPECT_EQ(window_notifier_test_access::read_word(notifier, 1), 0u);
    EXPECT_FALSE(notifier.written(1));
    EXPECT_TRUE(notifier.written(0));
    EXPECT_TRUE(notifier.written(2));

    window_notifier_test_access::write_word(notifier, 1, NVIDIA_BEGUN | 0x1234);
    EXPECT_TRUE(notifier.written(1));

    window_notifier_test_access::write_word(notifier, 1, NVIDIA_FINISHED | 0x3FFFFFFF);
    EXPECT_TRUE(notifier.written(1));

    window_notifier_test_access::write_word(notifier, 1, 0x3FFFFFFF);
    EXPECT_FALSE(notifier.written(1));

    window_notifier_test_access::write_word(notifier, 1, STATUS_3);
    EXPECT_FALSE(notifier.written(1));
}
