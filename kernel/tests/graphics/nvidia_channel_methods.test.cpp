// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "drivers/graphics/nvidia/channel_methods.h"

TEST_SUITE(nvidia_channel_methods);

using drivers::nvidia::channel_allocation_flags;
using drivers::nvidia::doorbell_token;
using drivers::nvidia::FILL_WORDS;
using drivers::nvidia::gpfifo_entry;
using drivers::nvidia::make_gpfifo_entry;
using drivers::nvidia::RELEASE_WORDS;
using drivers::nvidia::write_fill;
using drivers::nvidia::write_release;

// Words NVIDIA's encodings give, restated here so the tests hold the encoders to NVIDIA's layout rather than to the
// driver's: SEC_OP_INC_METHOD headers on subchannel 0 with the method's word offset and the value count
static constexpr uint32_t HEADER_SET_OBJECT           = 0x20010000;
static constexpr uint32_t HEADER_REMAP_CONST_A        = 0x200101C0;
static constexpr uint32_t HEADER_REMAP_COMPONENTS     = 0x200101C2;
static constexpr uint32_t HEADER_DST_PHYS_MODE        = 0x20010099;
static constexpr uint32_t HEADER_OFFSET_OUT_TWO       = 0x20020102;
static constexpr uint32_t HEADER_LINE_LENGTH_IN       = 0x20010106;
static constexpr uint32_t HEADER_LAUNCH_DMA           = 0x200100C0;
static constexpr uint32_t HEADER_WFI                  = 0x2001001E;
static constexpr uint32_t HEADER_MEM_OP_FOUR          = 0x2004000A;
static constexpr uint32_t HEADER_SEM_FIVE             = 0x20050017;
static constexpr uint32_t HEADER_NON_STALL_INTERRUPT  = 0x20010008;
static constexpr uint32_t AMPERE_DMA_COPY_B           = 0xC7B5;
static constexpr uint32_t REMAP_CONST_A_FOUR_BYTES    = 0x00030004;
static constexpr uint32_t LAUNCH_PHYSICAL_PITCH_REMAP = 0x04002586;
static constexpr uint32_t MEM_OP_D_MEMBAR             = 0x28000000;
static constexpr uint32_t SEM_EXECUTE_RELEASE_32BIT   = 0x00000001;

// A fill of 64 KiB at a video memory offset above 4 GiB sets the copy class, a constant remapped into every 4-byte
// element, the physical video memory target, the address, the element count and the launch
TEST(nvidia_channel_methods, fill_remaps_a_constant_into_every_4_byte_element) {
    uint32_t words[FILL_WORDS + 1] = {};
    words[FILL_WORDS] = 0xDEADBEEF;

    write_fill(words, AMPERE_DMA_COPY_B, 0x2'4000'1000ull, 0x10000, 0xA5A5'5A5A);

    static constexpr uint32_t expected[FILL_WORDS] = {
        HEADER_SET_OBJECT, AMPERE_DMA_COPY_B,
        HEADER_REMAP_CONST_A, 0xA5A5'5A5A,
        HEADER_REMAP_COMPONENTS, REMAP_CONST_A_FOUR_BYTES,
        HEADER_DST_PHYS_MODE, 0,
        HEADER_OFFSET_OUT_TWO, 0x2, 0x4000'1000,
        HEADER_LINE_LENGTH_IN, 0x4000,
        HEADER_LAUNCH_DMA, LAUNCH_PHYSICAL_PITCH_REMAP,
    };

    for (uint32_t i = 0; i < FILL_WORDS; i++) {
        EXPECT_EQ(words[i], expected[i]);
    }

    EXPECT_EQ(words[FILL_WORDS], 0xDEADBEEFu);
}

// A release waits for idle, makes earlier writes visible, writes the 32-bit payload, makes it visible and only then
// raises the nonstall interrupt
TEST(nvidia_channel_methods, release_orders_barriers_around_the_payload) {
    uint32_t words[RELEASE_WORDS + 1] = {};
    words[RELEASE_WORDS] = 0xDEADBEEF;

    write_release(words, 0xF1'2345'6788ull, 0x8000'0001);

    static constexpr uint32_t expected[RELEASE_WORDS] = {
        HEADER_WFI, 0,
        HEADER_MEM_OP_FOUR, 0, 0, 0, MEM_OP_D_MEMBAR,
        HEADER_SEM_FIVE, 0x2345'6788, 0xF1, 0x8000'0001, 0, SEM_EXECUTE_RELEASE_32BIT,
        HEADER_MEM_OP_FOUR, 0, 0, 0, MEM_OP_D_MEMBAR,
        HEADER_NON_STALL_INTERRUPT, 0,
    };

    for (uint32_t i = 0; i < RELEASE_WORDS; i++) {
        EXPECT_EQ(words[i], expected[i]);
    }

    EXPECT_EQ(words[RELEASE_WORDS], 0xDEADBEEFu);
}

// A GPFIFO entry carries the address's bits 31:2 in its first word, and bits 39:32 and the word count from bit 10 in
// its second, with an unconditional fetch, the main level and no sync
TEST(nvidia_channel_methods, gpfifo_entry_splits_address_and_length) {
    gpfifo_entry entry = make_gpfifo_entry(0x12'3456'7894ull, 20);

    EXPECT_EQ(entry.words[0], 0x3456'7894u);
    EXPECT_EQ(entry.words[1], 0x12u | (20u << 10));
}

// The doorbell's token holds the runlist from bit 16 and the channel id below it
TEST(nvidia_channel_methods, doorbell_token_holds_runlist_and_channel) {
    EXPECT_EQ(doorbell_token(1, 0), 0x0001'0000u);
    EXPECT_EQ(doorbell_token(8, 13), 0x0008'000Du);
}

// The kernel's channel 0 is privileged with USERD page and index 0, and an unprivileged channel 13 is denied physical
// copies and privileged MEM_OPs with USERD page 1 and index 5, both with scheduling delayed and the page pinned
TEST(nvidia_channel_methods, allocation_flags_pin_the_channel_id) {
    EXPECT_EQ(channel_allocation_flags(0, true), 0x0020'0060u);
    EXPECT_EQ(channel_allocation_flags(13, false), 0x0060'15C0u);
}
