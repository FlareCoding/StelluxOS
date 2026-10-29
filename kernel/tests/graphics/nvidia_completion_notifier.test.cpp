// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/completion_notifier.h"
#include "drivers/graphics/nvidia/display_channel.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/registers.h"
#include "hw/mmio.h"
#include "tests/memory/dma_test_access.h"

TEST_SUITE(nvidia_completion_notifier);

using drivers::nvidia::completion_notifier;
using drivers::nvidia::COMPLETION_NOTIFIER_BYTES;
using drivers::nvidia::COMPLETION_NOTIFIER_WAIT_NS;
using drivers::nvidia::display_channel;
using drivers::nvidia::ERR_NO_MEMORY;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::NS_PER_MS;
using drivers::nvidia::OK;
using drivers::nvidia::PUSH_BUFFER_BYTES;
using drivers::nvidia::registers;

namespace drivers::nvidia {

// Writes the notifier's first word as the display engine would
struct completion_notifier_test_access {
    static void write_word(completion_notifier& notifier, uint32_t word) {
        mmio::write32(notifier.m_page.virt, word);
    }
};

} // namespace drivers::nvidia

using drivers::nvidia::completion_notifier_test_access;

// NVIDIA's size of a display channel's completion notifier, its 3-second wait, and its first word's statuses in bits
// 31:30, restated so a mistake in the driver's copy is caught
static constexpr uint64_t NVIDIA_NOTIFIER_BYTES = 0x1000;
static constexpr uint64_t NVIDIA_WAIT_NS        = 3000000000;
static constexpr uint32_t NVIDIA_BEGUN          = 0x40000000;
static constexpr uint32_t NVIDIA_FINISHED       = 0x80000000;

// A first word with 3 in bits 31:30, a status neither begun nor finished
static constexpr uint32_t STATUS_3 = 0xC0000000;

// A card's full address width, and a limit `dma::alloc_pages` refuses
static constexpr uint8_t FULL_ADDRESS_BITS    = 64;
static constexpr uint8_t REFUSED_ADDRESS_BITS = 16;

// The core channel's push buffer and area, ordinary memory standing in for DMA pages and BAR0, GET's word in the area,
// a GET other than a fresh channel's PUT of 0, and how long the tests' waits last
alignas(4096) static uint32_t g_push_buffer[PUSH_BUFFER_BYTES / 4];
static uint32_t g_area[2];
static constexpr uint32_t GET_WORD       = 1;
static constexpr uint32_t GET_NOT_AT_PUT = 0x40;
static constexpr uint64_t TEST_WAIT_NS   = NS_PER_MS;

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

// Waits for the notifier with the log kept
static int32_t wait_logged(completion_notifier& notifier, display_channel& core, uint64_t deadline_ns) {
    s_notifier_log_bytes = 0;
    log::set_backend(&NOTIFIER_LOG_BACKEND);
    int32_t rc = notifier.wait_finished(core, deadline_ns);
    log::set_backend(nullptr);

    return rc;
}

// Allocates the notifier's page for a card with a full address width
static void allocate_notifier(completion_notifier& notifier) {
    dma::device_profile card = dma::device_profile_test_access::build(FULL_ADDRESS_BITS, true);
    ASSERT_EQ(notifier.allocate(card), OK);
}

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

// Clearing writes NVIDIA's not-begun status, a first word of 0, over a finished one
TEST(nvidia_completion_notifier, clear_writes_the_not_begun_status) {
    completion_notifier notifier;
    allocate_notifier(notifier);
    completion_notifier_test_access::write_word(notifier, NVIDIA_FINISHED | 0x1);

    notifier.clear();

    EXPECT_EQ(notifier.status_word(), 0u);
}

// `COMPLETION_NOTIFIER_WAIT_NS` is NVIDIA's 3 seconds, and a finished status ends the wait with `OK` at once whatever
// the word's other bits, even with its deadline passed and GET not at PUT
TEST(nvidia_completion_notifier, finished_status_ends_the_wait) {
    EXPECT_EQ(COMPLETION_NOTIFIER_WAIT_NS, NVIDIA_WAIT_NS);

    completion_notifier notifier;
    allocate_notifier(notifier);
    display_channel core(g_push_buffer, registers(reinterpret_cast<uintptr_t>(g_area)));
    g_area[GET_WORD] = GET_NOT_AT_PUT;
    completion_notifier_test_access::write_word(notifier, NVIDIA_FINISHED | 0x3FFFFFFF);

    EXPECT_EQ(notifier.wait_finished(core, 0), OK);
}

// A begun status or status 3 is not finished, so with GET not at PUT the wait gives up only past its deadline with
// `ERR_TIMEOUT`, logging the word
TEST(nvidia_completion_notifier, unfinished_status_with_get_not_at_put_times_out) {
    const uint32_t words[] = {NVIDIA_BEGUN, STATUS_3};
    const char*    logged[] = {"did not read finished: 0x40000000", "did not read finished: 0xC0000000"};
    for (uint32_t i = 0; i < 2; i++) {
        completion_notifier notifier;
        allocate_notifier(notifier);
        display_channel core(g_push_buffer, registers(reinterpret_cast<uintptr_t>(g_area)));
        g_area[GET_WORD] = GET_NOT_AT_PUT;
        completion_notifier_test_access::write_word(notifier, words[i]);

        uint64_t deadline_ns = clock::now_ns() + TEST_WAIT_NS;
        EXPECT_EQ(wait_logged(notifier, core, deadline_ns), ERR_TIMEOUT);
        EXPECT_GT(clock::now_ns(), deadline_ns);
        EXPECT_TRUE(notifier_log_contains(logged[i]));
        EXPECT_FALSE(notifier_log_contains("a lost notification"));
    }
}

// A notifier not begun or only begun at the deadline with GET at PUT is a lost notification, which the wait takes as
// finished only past its deadline, logging the word
TEST(nvidia_completion_notifier, lost_notification_with_get_at_put_ends_the_wait) {
    const uint32_t words[] = {0, NVIDIA_BEGUN};
    const char*    logged[] = {"reads 0x00000000, a lost notification", "reads 0x40000000, a lost notification"};
    for (uint32_t i = 0; i < 2; i++) {
        completion_notifier notifier;
        allocate_notifier(notifier);
        display_channel core(g_push_buffer, registers(reinterpret_cast<uintptr_t>(g_area)));
        g_area[GET_WORD] = 0;
        completion_notifier_test_access::write_word(notifier, words[i]);

        uint64_t deadline_ns = clock::now_ns() + TEST_WAIT_NS;
        EXPECT_EQ(wait_logged(notifier, core, deadline_ns), OK);
        EXPECT_GT(clock::now_ns(), deadline_ns);
        EXPECT_EQ(notifier.status_word(), words[i]);
        EXPECT_TRUE(notifier_log_contains(logged[i]));
        EXPECT_FALSE(notifier_log_contains("did not read finished"));
    }
}
