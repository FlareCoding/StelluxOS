#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/registers.h"

TEST_SUITE(nvidia);

using drivers::nvidia::chip_info;
using drivers::nvidia::decode_chip_id;
using drivers::nvidia::decode_chip_revision;
using drivers::nvidia::find_chip;
using drivers::nvidia::PMC_BOOT_0;
using drivers::nvidia::registers;

// PMC_BOOT_0 as an RTX 3080 reports it, a GA102 at revision A1
static constexpr uint32_t RTX_3080_BOOT_0 = 0xb72000a1;
static constexpr uint16_t GA102_CHIP_ID = 0x172;
static constexpr uint8_t RTX_3080_REVISION = 0xa1;

// The RTX 3080's identity decodes to the GA102 entry of the chip table
TEST(nvidia, rtx_3080_decodes_as_ga102) {
    EXPECT_EQ(decode_chip_id(RTX_3080_BOOT_0), GA102_CHIP_ID);
    EXPECT_EQ(decode_chip_revision(RTX_3080_BOOT_0), RTX_3080_REVISION);

    const chip_info* chip = find_chip(GA102_CHIP_ID);
    ASSERT_NOT_NULL(chip);
    EXPECT_EQ(chip->id, GA102_CHIP_ID);
    EXPECT_STREQ(chip->name, "GA102");
}

// GA104 shares GA102's architecture but has no chip table entry
static constexpr uint16_t GA104_CHIP_ID = 0x174;

// Sharing a listed chip's architecture does not give a chip an entry
TEST(nvidia, ga104_has_no_entry) {
    EXPECT_NULL(find_chip(GA104_CHIP_ID));
}

// Large enough that a read counting words instead of bytes stays inside the
// array, so that bug fails the test instead of reading past it
static constexpr size_t FAKE_BAR0_WORDS = 16;
static constexpr uint32_t THIRD_WORD_OFFSET = 8;
static constexpr uint32_t THIRD_WORD_VALUE = 0x0000c0de;

// Offsets into a `registers` view count bytes, so offset 8 reads the third 32-bit word
TEST(nvidia, registers_view_reads_at_byte_offsets) {
    uint32_t fake_bar0[FAKE_BAR0_WORDS] = {RTX_3080_BOOT_0, 0, THIRD_WORD_VALUE};
    registers regs(reinterpret_cast<uintptr_t>(fake_bar0));

    EXPECT_EQ(regs.read(PMC_BOOT_0), RTX_3080_BOOT_0);
    EXPECT_EQ(regs.read(THIRD_WORD_OFFSET), THIRD_WORD_VALUE);
}
