// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#include "stlx_unit_test.h"
#include "drivers/graphics/nvidia/display_vertical_blank.h"

TEST_SUITE(nvidia_display_vertical_blank);

using drivers::nvidia::disable_vertical_blank;
using drivers::nvidia::enable_vertical_blank;
using drivers::nvidia::pending_vertical_blanks;
using drivers::nvidia::raster_frame;
using drivers::nvidia::raster_line;
using drivers::nvidia::raster_position;
using drivers::nvidia::registers;
using drivers::nvidia::take_vertical_blank;

// A head's timing event, interrupt status and interrupt enable, the display's interrupt dispatch and a head's raster
// position, as NVIDIA's headers give them, and LAST_DATA's bit, which written to the event clears it
static constexpr uint32_t NVIDIA_HEAD_TIMING_EVENT  = 0x611800;
static constexpr uint32_t NVIDIA_HEAD_TIMING_STATUS = 0x611C00;
static constexpr uint32_t NVIDIA_HEAD_TIMING_ENABLE = 0x611D80;
static constexpr uint32_t NVIDIA_INTERRUPT_DISPATCH = 0x611EC0;
static constexpr uint32_t NVIDIA_RG_DPCA            = 0x616330;
static constexpr uint32_t NVIDIA_RG_DPCA_STRIDE     = 0x800;
static constexpr uint32_t NVIDIA_LAST_DATA          = 1u << 1;

// The words standing in for BAR0 from head 0's timing event to the end of head 3's raster position
static constexpr uint32_t REGISTERS_START = NVIDIA_HEAD_TIMING_EVENT;
static constexpr uint32_t REGISTERS_END   = NVIDIA_RG_DPCA + 3 * NVIDIA_RG_DPCA_STRIDE + sizeof(uint32_t);
static uint32_t g_registers[(REGISTERS_END - REGISTERS_START) / sizeof(uint32_t)];

// A word no call writes, so a write that lands on it fails
static constexpr uint32_t STALE_WORD = 0xA5A5A5A5;

static constexpr uint8_t HEAD = 2;

// A view of BAR0 whose registers from `REGISTERS_START` are `g_registers`
static registers test_registers() {
    return registers(reinterpret_cast<uintptr_t>(g_registers) - REGISTERS_START);
}

// The word of `g_registers` standing in for the register at `offset`
static uint32_t& word_at(uint32_t offset) {
    return g_registers[(offset - REGISTERS_START) / sizeof(uint32_t)];
}

// A head's word, `HEAD`'s by default, in the array of per-head words starting at `first`
static uint32_t& head_word(uint32_t first, uint8_t head = HEAD) {
    return word_at(first + head * sizeof(uint32_t));
}

// An enable word whose other bits are mixed, with LAST_DATA clear
static constexpr uint32_t MIXED_ENABLE = 0x5A5A5A58;

// Enabling writes LAST_DATA alone to the head's event, clearing it, and keeps the enable's other bits, as disabling
// does, and neither touches another head's words
TEST(nvidia_display_vertical_blank, enable_and_disable_clear_the_event_and_keep_the_enables_other_bits) {
    registers regs = test_registers();
    head_word(NVIDIA_HEAD_TIMING_EVENT) = STALE_WORD;
    head_word(NVIDIA_HEAD_TIMING_ENABLE) = ~NVIDIA_LAST_DATA;
    head_word(NVIDIA_HEAD_TIMING_EVENT, HEAD + 1) = STALE_WORD;
    head_word(NVIDIA_HEAD_TIMING_ENABLE, HEAD + 1) = STALE_WORD;

    enable_vertical_blank(regs, HEAD);
    EXPECT_EQ(head_word(NVIDIA_HEAD_TIMING_EVENT), NVIDIA_LAST_DATA);
    EXPECT_EQ(head_word(NVIDIA_HEAD_TIMING_ENABLE), 0xFFFFFFFFu);

    head_word(NVIDIA_HEAD_TIMING_EVENT) = STALE_WORD;
    disable_vertical_blank(regs, HEAD);
    EXPECT_EQ(head_word(NVIDIA_HEAD_TIMING_EVENT), NVIDIA_LAST_DATA);
    EXPECT_EQ(head_word(NVIDIA_HEAD_TIMING_ENABLE), ~NVIDIA_LAST_DATA);

    head_word(NVIDIA_HEAD_TIMING_ENABLE) = MIXED_ENABLE;
    enable_vertical_blank(regs, HEAD);
    EXPECT_EQ(head_word(NVIDIA_HEAD_TIMING_ENABLE), MIXED_ENABLE | NVIDIA_LAST_DATA);
    disable_vertical_blank(regs, HEAD);
    EXPECT_EQ(head_word(NVIDIA_HEAD_TIMING_ENABLE), MIXED_ENABLE);

    EXPECT_EQ(head_word(NVIDIA_HEAD_TIMING_EVENT, HEAD + 1), STALE_WORD);
    EXPECT_EQ(head_word(NVIDIA_HEAD_TIMING_ENABLE, HEAD + 1), STALE_WORD);
}

// A head whose status shows LAST_DATA, alone or among every other bit, has its event cleared and is taken, while one
// whose status shows every other bit keeps its event and is not
TEST(nvidia_display_vertical_blank, take_clears_the_event_only_with_last_data_pending) {
    registers regs = test_registers();
    head_word(NVIDIA_HEAD_TIMING_EVENT) = STALE_WORD;
    head_word(NVIDIA_HEAD_TIMING_STATUS) = ~NVIDIA_LAST_DATA;

    EXPECT_FALSE(take_vertical_blank(regs, HEAD));
    EXPECT_EQ(head_word(NVIDIA_HEAD_TIMING_EVENT), STALE_WORD);

    uint32_t taken_statuses[] = {NVIDIA_LAST_DATA, 0xFFFFFFFF};
    for (uint32_t status : taken_statuses) {
        head_word(NVIDIA_HEAD_TIMING_EVENT) = STALE_WORD;
        head_word(NVIDIA_HEAD_TIMING_STATUS) = status;

        EXPECT_TRUE(take_vertical_blank(regs, HEAD));
        EXPECT_EQ(head_word(NVIDIA_HEAD_TIMING_EVENT), NVIDIA_LAST_DATA);
    }
}

// The pending heads are the dispatch word whole, and a head's raster position is its own register in its 2 KiB block,
// split into the line in bits 15:0 and the frame count in bits 31:16
TEST(nvidia_display_vertical_blank, pending_heads_and_raster_position_read_their_registers) {
    registers regs = test_registers();
    word_at(NVIDIA_INTERRUPT_DISPATCH) = 0x80000005;
    word_at(NVIDIA_RG_DPCA + HEAD * NVIDIA_RG_DPCA_STRIDE) = 0xBEEF0438;
    word_at(NVIDIA_RG_DPCA + (HEAD + 1) * NVIDIA_RG_DPCA_STRIDE) = STALE_WORD;

    EXPECT_EQ(pending_vertical_blanks(regs), 0x80000005u);

    uint32_t position = raster_position(regs, HEAD);
    EXPECT_EQ(position, 0xBEEF0438u);
    EXPECT_EQ(raster_line(position), 0x0438u);
    EXPECT_EQ(raster_frame(position), 0xBEEFu);
}
