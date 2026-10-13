#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "drivers/graphics/nvidia/chip.h"

TEST_SUITE(nvidia);

using drivers::nvidia::chip_info;
using drivers::nvidia::decode_chip_id;
using drivers::nvidia::decode_chip_revision;
using drivers::nvidia::find_chip;

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
