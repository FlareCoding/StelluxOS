// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "clock/clock.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/protected_memory.h"
#include "drivers/graphics/nvidia/registers.h"
#include "mm/heap.h"

TEST_SUITE(nvidia_protected_memory);

using drivers::nvidia::BOOT_FIRMWARE_TIMEOUT_NS;
using drivers::nvidia::check_frts_result;
using drivers::nvidia::check_fwsec_sb_result;
using drivers::nvidia::chip_gsp_reservation;
using drivers::nvidia::ERR_BAD_STATE;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::frts_region;
using drivers::nvidia::lay_out_protected_memory;
using drivers::nvidia::OK;
using drivers::nvidia::place_frts_region;
using drivers::nvidia::protected_memory_layout;
using drivers::nvidia::registers;
using drivers::nvidia::wait_for_boot_firmware;

// Registers and fields the steps read, restated here so the tests hold the steps to NVIDIA's layout rather than
// to their own constants
static constexpr uint32_t BOOT_PROGRESS_READ_PROTECTION = 0x118128;
static constexpr uint32_t BOOT_PROGRESS                 = 0x118234;
static constexpr uint32_t GSP_FALCON_CPUCTL             = 0x110100;
static constexpr uint32_t FRTS_ERROR_SCRATCH            = 0x1438;
static constexpr uint32_t SB_ERROR_SCRATCH              = 0x1454;
static constexpr uint32_t PROTECTED_MEMORY_LOWER_BOUND  = 0x1FA824;
static constexpr uint32_t PROTECTED_MEMORY_UPPER_BOUND  = 0x1FA828;
static constexpr uint32_t USABLE_VIDEO_MEMORY_MIB       = 0x1183A4;
static constexpr uint32_t DISPLAY_FUSE                  = 0x820C04;
static constexpr uint32_t VGA_WORKSPACE_BASE            = 0x625F04;

static constexpr uint32_t READABLE_AT_LEVEL_0       = 0x1;
static constexpr uint32_t BOOT_PROGRESS_COMPLETE    = 0xFF;
static constexpr uint32_t DISPLAY_DISABLED          = 0x1;
static constexpr uint32_t VGA_WORKSPACE_VALID       = 0x8;
static constexpr uint32_t VGA_WORKSPACE_START_SHIFT = 8;

static constexpr uint64_t MIB                       = 0x100000;
static constexpr uint64_t FRTS_REGION_BYTES         = 0x100000;
static constexpr uint64_t VGA_WORKSPACE_UNIT_BYTES  = 0x10000;
static constexpr uint64_t VGA_WORKSPACE_MAX_BYTES   = 0x100000;
static constexpr uint64_t VALID_VGA_WORKSPACE_BYTES = 0x20000;

// NVIDIA's deadline for the boot firmware: 50 ms to start and 2 s to finish
static constexpr uint64_t NVIDIA_BOOT_FIRMWARE_TIMEOUT_NS = 2050000000;

// A zeroed fake of BAR0, all 16 MiB of it
struct fake_bar0 {
    static constexpr size_t BYTES = 0x1000000;

    uint8_t* bytes = static_cast<uint8_t*>(heap::uzalloc(BYTES));
    registers regs{reinterpret_cast<uintptr_t>(bytes)};

    ~fake_bar0() { heap::ufree(bytes); }

    void write(uint32_t offset, uint32_t value) { *reinterpret_cast<uint32_t*>(bytes + offset) = value; }
};

// The boot firmware's registers as an RTX 3080 reports them once its boot firmware has finished
static constexpr uint32_t RTX_3080_READ_PROTECTION = 0x8B8F;
static constexpr uint32_t RTX_3080_BOOT_PROGRESS   = 0x3FF;
static constexpr uint32_t RTX_3080_GSP_CPUCTL      = 0x10;

static void put_finished_boot_firmware(fake_bar0& bar0) {
    bar0.write(BOOT_PROGRESS_READ_PROTECTION, RTX_3080_READ_PROTECTION);
    bar0.write(BOOT_PROGRESS, RTX_3080_BOOT_PROGRESS);
    bar0.write(GSP_FALCON_CPUCTL, RTX_3080_GSP_CPUCTL);
}

// The RTX 3080's boot firmware has finished, and the registers are read even for a timeout of 0
TEST(nvidia_protected_memory, rtx_3080_boot_firmware_has_finished) {
    fake_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    put_finished_boot_firmware(bar0);

    EXPECT_EQ(wait_for_boot_firmware(bar0.regs, 0), OK);
}

// Each condition of a finished boot firmware is needed on its own, and only its own bits count
TEST(nvidia_protected_memory, boot_firmware_needs_every_condition) {
    fake_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    put_finished_boot_firmware(bar0);
    bar0.write(BOOT_PROGRESS_READ_PROTECTION, RTX_3080_READ_PROTECTION & ~READABLE_AT_LEVEL_0);
    EXPECT_EQ(wait_for_boot_firmware(bar0.regs, 0), ERR_TIMEOUT);

    put_finished_boot_firmware(bar0);
    bar0.write(BOOT_PROGRESS_READ_PROTECTION, READABLE_AT_LEVEL_0);
    EXPECT_EQ(wait_for_boot_firmware(bar0.regs, 0), OK);

    put_finished_boot_firmware(bar0);
    bar0.write(BOOT_PROGRESS, BOOT_PROGRESS_COMPLETE - 1);
    EXPECT_EQ(wait_for_boot_firmware(bar0.regs, 0), ERR_TIMEOUT);

    bar0.write(BOOT_PROGRESS, BOOT_PROGRESS_COMPLETE >> 1);
    EXPECT_EQ(wait_for_boot_firmware(bar0.regs, 0), ERR_TIMEOUT);

    put_finished_boot_firmware(bar0);
    bar0.write(GSP_FALCON_CPUCTL, ~RTX_3080_GSP_CPUCTL);
    EXPECT_EQ(wait_for_boot_firmware(bar0.regs, 0), ERR_TIMEOUT);
}

// Bring-up waits as long as NVIDIA's driver does
TEST(nvidia_protected_memory, boot_firmware_timeout_is_nvidias) {
    EXPECT_EQ(BOOT_FIRMWARE_TIMEOUT_NS, NVIDIA_BOOT_FIRMWARE_TIMEOUT_NS);
}

static constexpr uint64_t SHORT_TIMEOUT_NS = 1000000;
static constexpr uint64_t ONE_SECOND_NS    = 1000000000;

// A boot firmware that never finishes keeps the wait going for the whole timeout and not much longer
TEST(nvidia_protected_memory, boot_firmware_wait_lasts_its_timeout) {
    fake_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    uint64_t start = clock::now_ns();
    EXPECT_EQ(wait_for_boot_firmware(bar0.regs, SHORT_TIMEOUT_NS), ERR_TIMEOUT);
    uint64_t elapsed = clock::now_ns() - start;
    EXPECT_GE(elapsed, SHORT_TIMEOUT_NS);
    EXPECT_LT(elapsed, ONE_SECOND_NS);
}

// The layout registers as an RTX 3080 booted on its own framebuffer reports them, with its display enabled and
// its workspace register not valid
static constexpr uint32_t RTX_3080_VIDEO_MEMORY_MIB = 10240;
static constexpr uint32_t RTX_3080_DISPLAY_FUSE     = 0x0;
static constexpr uint32_t RTX_3080_VGA_WORKSPACE    = 0x1;

static void put_layout(fake_bar0& bar0, uint32_t video_memory_mib, uint32_t display_fuse, uint32_t vga_workspace) {
    bar0.write(USABLE_VIDEO_MEMORY_MIB, video_memory_mib);
    bar0.write(DISPLAY_FUSE, display_fuse);
    bar0.write(VGA_WORKSPACE_BASE, vga_workspace);
}

// The RTX 3080's VGA workspace takes the last 1 MiB of its 10 GiB, and the FRTS region the 1 MiB below
TEST(nvidia_protected_memory, rtx_3080_frts_region_is_placed) {
    fake_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    put_layout(bar0, RTX_3080_VIDEO_MEMORY_MIB, RTX_3080_DISPLAY_FUSE, RTX_3080_VGA_WORKSPACE);

    frts_region region = {};
    ASSERT_EQ(place_frts_region(bar0.regs, region), OK);
    EXPECT_EQ(region.video_memory_size, 0x280000000u);
    EXPECT_EQ(region.vga_workspace_offset, 0x27FF00000u);
    EXPECT_EQ(region.offset, 0x27FE00000u);
    EXPECT_TRUE(region.has_display);
}

static constexpr uint32_t VIDEO_MEMORY_MIB = 64;
static constexpr uint64_t VIDEO_MEMORY_BYTES = VIDEO_MEMORY_MIB * MIB;

// The workspace register of a valid workspace starting at `start`, which must be a multiple of 64 KiB
static uint32_t vga_workspace_at(uint64_t start) {
    return (static_cast<uint32_t>(start / VGA_WORKSPACE_UNIT_BYTES) << VGA_WORKSPACE_START_SHIFT) | VGA_WORKSPACE_VALID;
}

// Without a display or a valid workspace register, the VGA workspace takes the last 1 MiB, and the region reports
// whether the chip has a display
TEST(nvidia_protected_memory, workspace_without_a_display_or_a_valid_register_takes_the_last_mib) {
    fake_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    uint32_t last_unit_register = vga_workspace_at(VIDEO_MEMORY_BYTES - VGA_WORKSPACE_UNIT_BYTES);

    put_layout(bar0, VIDEO_MEMORY_MIB, DISPLAY_DISABLED, last_unit_register);
    frts_region region = {};
    ASSERT_EQ(place_frts_region(bar0.regs, region), OK);
    EXPECT_EQ(region.vga_workspace_offset, VIDEO_MEMORY_BYTES - VGA_WORKSPACE_MAX_BYTES);
    EXPECT_EQ(region.offset, VIDEO_MEMORY_BYTES - VGA_WORKSPACE_MAX_BYTES - FRTS_REGION_BYTES);
    EXPECT_FALSE(region.has_display);

    put_layout(bar0, VIDEO_MEMORY_MIB, 0xFFFFFFFF, last_unit_register);
    region = {};
    ASSERT_EQ(place_frts_region(bar0.regs, region), OK);
    EXPECT_EQ(region.vga_workspace_offset, VIDEO_MEMORY_BYTES - VGA_WORKSPACE_MAX_BYTES);
    EXPECT_FALSE(region.has_display);

    put_layout(bar0, VIDEO_MEMORY_MIB, 0, last_unit_register & ~VGA_WORKSPACE_VALID);
    region = {};
    ASSERT_EQ(place_frts_region(bar0.regs, region), OK);
    EXPECT_EQ(region.vga_workspace_offset, VIDEO_MEMORY_BYTES - VGA_WORKSPACE_MAX_BYTES);
    EXPECT_TRUE(region.has_display);
}

// A valid workspace register gives the last 128 KiB wherever it points, as NVIDIA's driver does when more than
// 4 GiB is usable, and any display fuse with bit 0 clear counts as a display
TEST(nvidia_protected_memory, workspace_with_a_valid_register_takes_the_last_128_kib) {
    fake_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    uint64_t rtx_3080_video_memory_bytes = RTX_3080_VIDEO_MEMORY_MIB * MIB;

    uint64_t starts[] = {
        0,
        rtx_3080_video_memory_bytes - VGA_WORKSPACE_MAX_BYTES - VGA_WORKSPACE_UNIT_BYTES,
        rtx_3080_video_memory_bytes - VGA_WORKSPACE_MAX_BYTES,
        rtx_3080_video_memory_bytes - VGA_WORKSPACE_UNIT_BYTES,
        rtx_3080_video_memory_bytes,
        rtx_3080_video_memory_bytes + MIB,
    };

    for (uint64_t start : starts) {
        put_layout(bar0, RTX_3080_VIDEO_MEMORY_MIB, 0, vga_workspace_at(start));
        frts_region region = {};
        ASSERT_EQ(place_frts_region(bar0.regs, region), OK);
        EXPECT_EQ(region.vga_workspace_offset, rtx_3080_video_memory_bytes - VALID_VGA_WORKSPACE_BYTES);
        EXPECT_EQ(region.offset, rtx_3080_video_memory_bytes - VALID_VGA_WORKSPACE_BYTES - FRTS_REGION_BYTES);
    }

    put_layout(bar0, RTX_3080_VIDEO_MEMORY_MIB, ~DISPLAY_DISABLED, VGA_WORKSPACE_VALID);
    frts_region region = {};
    ASSERT_EQ(place_frts_region(bar0.regs, region), OK);
    EXPECT_EQ(region.vga_workspace_offset, rtx_3080_video_memory_bytes - VALID_VGA_WORKSPACE_BYTES);
    EXPECT_TRUE(region.has_display);
}

// Every bit of the usable size counts, past 32 bits of bytes
TEST(nvidia_protected_memory, usable_size_uses_its_whole_field) {
    fake_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    put_layout(bar0, 0xFFFFFFFF, DISPLAY_DISABLED, 0);
    frts_region region = {};
    ASSERT_EQ(place_frts_region(bar0.regs, region), OK);
    EXPECT_EQ(region.video_memory_size, 0xFFFFFFFF * MIB);
    EXPECT_EQ(region.offset, 0xFFFFFFFF * MIB - VGA_WORKSPACE_MAX_BYTES - FRTS_REGION_BYTES);
}

// Protected memory set up by an earlier bring-up is refused, which only bits 31 to 4 of its bound show
TEST(nvidia_protected_memory, protected_memory_already_set_up_is_refused) {
    fake_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    put_layout(bar0, VIDEO_MEMORY_MIB, DISPLAY_DISABLED, 0);

    bar0.write(PROTECTED_MEMORY_UPPER_BOUND, 0x10);
    frts_region region = {.video_memory_size = 1, .vga_workspace_offset = 2, .offset = 3, .has_display = true};
    EXPECT_EQ(place_frts_region(bar0.regs, region), ERR_BAD_STATE);
    EXPECT_EQ(region.video_memory_size, 1u);
    EXPECT_EQ(region.vga_workspace_offset, 2u);
    EXPECT_EQ(region.offset, 3u);
    EXPECT_TRUE(region.has_display);

    bar0.write(PROTECTED_MEMORY_UPPER_BOUND, 0x80000000);
    EXPECT_EQ(place_frts_region(bar0.regs, region), ERR_BAD_STATE);

    bar0.write(PROTECTED_MEMORY_UPPER_BOUND, 0xF);
    EXPECT_EQ(place_frts_region(bar0.regs, region), OK);
}

// The region must start above offset 0, below a workspace in the last 1 MiB of usable video memory
TEST(nvidia_protected_memory, region_must_start_above_offset_0) {
    fake_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    frts_region region = {};

    put_layout(bar0, 3, DISPLAY_DISABLED, 0);
    ASSERT_EQ(place_frts_region(bar0.regs, region), OK);
    EXPECT_EQ(region.offset, MIB);

    put_layout(bar0, 2, 0, VGA_WORKSPACE_VALID);
    ASSERT_EQ(place_frts_region(bar0.regs, region), OK);
    EXPECT_EQ(region.offset, 2 * MIB - VALID_VGA_WORKSPACE_BYTES - FRTS_REGION_BYTES);

    put_layout(bar0, 2, DISPLAY_DISABLED, 0);
    EXPECT_EQ(place_frts_region(bar0.regs, region), ERR_BAD_STATE);

    put_layout(bar0, 1, 0, VGA_WORKSPACE_VALID);
    EXPECT_EQ(place_frts_region(bar0.regs, region), ERR_BAD_STATE);

    put_layout(bar0, 0, DISPLAY_DISABLED, 0);
    EXPECT_EQ(place_frts_region(bar0.regs, region), ERR_BAD_STATE);
}

// FWSEC's result as an RTX 3080 reports it once FWSEC fenced off the FRTS region of its layout
static constexpr uint32_t RTX_3080_FRTS_ERROR_SCRATCH           = 0x0;
static constexpr uint32_t RTX_3080_PROTECTED_MEMORY_LOWER_BOUND = 0x27FE000;
static constexpr uint32_t RTX_3080_PROTECTED_MEMORY_UPPER_BOUND = 0x27FEE00;

static constexpr frts_region RTX_3080_FRTS_REGION = {
    .video_memory_size = 0x280000000,
    .vga_workspace_offset = 0x27FF00000,
    .offset = 0x27FE00000,
    .has_display = true,
};

static void put_rtx_3080_frts_result(fake_bar0& bar0) {
    bar0.write(FRTS_ERROR_SCRATCH, RTX_3080_FRTS_ERROR_SCRATCH);
    bar0.write(PROTECTED_MEMORY_LOWER_BOUND, RTX_3080_PROTECTED_MEMORY_LOWER_BOUND);
    bar0.write(PROTECTED_MEMORY_UPPER_BOUND, RTX_3080_PROTECTED_MEMORY_UPPER_BOUND);
}

// The FRTS region placed for the RTX 3080's layout is the one its FWSEC fenced off
TEST(nvidia_protected_memory, rtx_3080_frts_result_checks_out) {
    fake_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    put_layout(bar0, RTX_3080_VIDEO_MEMORY_MIB, RTX_3080_DISPLAY_FUSE, RTX_3080_VGA_WORKSPACE);

    frts_region region = {};
    ASSERT_EQ(place_frts_region(bar0.regs, region), OK);

    put_rtx_3080_frts_result(bar0);
    EXPECT_EQ(check_frts_result(bar0.regs, region), OK);
}

// Bits 31 to 16 of the scratch register hold FWSEC's FRTS error code, and any code but 0 is refused
TEST(nvidia_protected_memory, nonzero_frts_error_code_is_refused) {
    fake_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    put_rtx_3080_frts_result(bar0);

    bar0.write(FRTS_ERROR_SCRATCH, 0x10000);
    EXPECT_EQ(check_frts_result(bar0.regs, RTX_3080_FRTS_REGION), ERR_BAD_STATE);

    bar0.write(FRTS_ERROR_SCRATCH, 0x80000000);
    EXPECT_EQ(check_frts_result(bar0.regs, RTX_3080_FRTS_REGION), ERR_BAD_STATE);

    bar0.write(FRTS_ERROR_SCRATCH, 0xFFFF);
    EXPECT_EQ(check_frts_result(bar0.regs, RTX_3080_FRTS_REGION), OK);
}

// The RTX 3080's bounds before FWSEC ran, with protected memory not set up though the lower bound holds a value
static constexpr uint32_t RTX_3080_PROTECTED_MEMORY_LOWER_BOUND_BEFORE_FWSEC = 0x1FFFFE00;
static constexpr uint32_t RTX_3080_PROTECTED_MEMORY_UPPER_BOUND_BEFORE_FWSEC = 0x0;

// Protected memory must be set up, which only bits 31 to 4 of its upper bound show, and the RTX 3080's bounds from
// before FWSEC ran are refused
TEST(nvidia_protected_memory, frts_result_needs_protected_memory_set_up) {
    fake_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    put_rtx_3080_frts_result(bar0);

    bar0.write(PROTECTED_MEMORY_UPPER_BOUND, 0);
    EXPECT_EQ(check_frts_result(bar0.regs, RTX_3080_FRTS_REGION), ERR_BAD_STATE);

    bar0.write(PROTECTED_MEMORY_UPPER_BOUND, 0xF);
    EXPECT_EQ(check_frts_result(bar0.regs, RTX_3080_FRTS_REGION), ERR_BAD_STATE);

    bar0.write(PROTECTED_MEMORY_UPPER_BOUND, 0x10);
    EXPECT_EQ(check_frts_result(bar0.regs, RTX_3080_FRTS_REGION), OK);

    bar0.write(PROTECTED_MEMORY_UPPER_BOUND, 0x80000000);
    EXPECT_EQ(check_frts_result(bar0.regs, RTX_3080_FRTS_REGION), OK);

    bar0.write(PROTECTED_MEMORY_LOWER_BOUND, RTX_3080_PROTECTED_MEMORY_LOWER_BOUND_BEFORE_FWSEC);
    bar0.write(PROTECTED_MEMORY_UPPER_BOUND, RTX_3080_PROTECTED_MEMORY_UPPER_BOUND_BEFORE_FWSEC);
    EXPECT_EQ(check_frts_result(bar0.regs, RTX_3080_FRTS_REGION), ERR_BAD_STATE);
}

// One 4 KiB unit in a bound's address field, which bits 31 to 4 hold
static constexpr uint32_t BOUND_ADDRESS_UNIT = 0x10;

// Protected memory must start at the region: bits 31 to 4 of the lower bound must hold its offset in 4 KiB units
TEST(nvidia_protected_memory, protected_memory_must_start_at_the_region) {
    fake_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    put_rtx_3080_frts_result(bar0);

    bar0.write(PROTECTED_MEMORY_LOWER_BOUND, RTX_3080_PROTECTED_MEMORY_LOWER_BOUND + BOUND_ADDRESS_UNIT);
    EXPECT_EQ(check_frts_result(bar0.regs, RTX_3080_FRTS_REGION), ERR_BAD_STATE);

    bar0.write(PROTECTED_MEMORY_LOWER_BOUND, RTX_3080_PROTECTED_MEMORY_LOWER_BOUND - BOUND_ADDRESS_UNIT);
    EXPECT_EQ(check_frts_result(bar0.regs, RTX_3080_FRTS_REGION), ERR_BAD_STATE);

    bar0.write(PROTECTED_MEMORY_LOWER_BOUND, RTX_3080_PROTECTED_MEMORY_LOWER_BOUND | 0x80000000);
    EXPECT_EQ(check_frts_result(bar0.regs, RTX_3080_FRTS_REGION), ERR_BAD_STATE);

    bar0.write(PROTECTED_MEMORY_LOWER_BOUND, RTX_3080_PROTECTED_MEMORY_LOWER_BOUND | 0xF);
    EXPECT_EQ(check_frts_result(bar0.regs, RTX_3080_FRTS_REGION), OK);
}

// A region just below 1 TiB, whose offset in 4 KiB units reaches bit 31 of the lower bound
TEST(nvidia_protected_memory, lower_bound_uses_its_whole_field) {
    fake_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    frts_region region = {
        .video_memory_size = 0x10000000000,
        .vga_workspace_offset = 0xFFFFF00000,
        .offset = 0xFFFFE00000,
        .has_display = true,
    };

    bar0.write(PROTECTED_MEMORY_LOWER_BOUND, 0xFFFFE000);
    bar0.write(PROTECTED_MEMORY_UPPER_BOUND, 0xFFFFEE00);
    EXPECT_EQ(check_frts_result(bar0.regs, region), OK);
}

// The region's offset is compared in 4 KiB units kept to 32 bits, so the RTX 3080's region raised by 8 TiB, whose
// offset in 4 KiB units matches the RTX 3080's lower bound in its low 31 bits only, is refused
TEST(nvidia_protected_memory, region_offset_is_compared_in_32_bits) {
    fake_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    put_rtx_3080_frts_result(bar0);
    frts_region region = {
        .video_memory_size = 0x80280000000,
        .vga_workspace_offset = 0x8027FF00000,
        .offset = 0x8027FE00000,
        .has_display = true,
    };

    EXPECT_EQ(check_frts_result(bar0.regs, region), ERR_BAD_STATE);
}

// FWSEC's result for the SB command with no error code, and the boot firmware's registers as an RTX 3080 reports them
// once its boot firmware has finished
static void put_sb_result(fake_bar0& bar0) {
    bar0.write(BOOT_PROGRESS_READ_PROTECTION, RTX_3080_READ_PROTECTION);
    bar0.write(BOOT_PROGRESS, RTX_3080_BOOT_PROGRESS);
    bar0.write(SB_ERROR_SCRATCH, 0);
}

// The progress register must be open to reading and read complete, each on its own, and only their own bits count
TEST(nvidia_protected_memory, sb_result_needs_the_boot_progress_readable_and_complete) {
    fake_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    put_sb_result(bar0);
    EXPECT_EQ(check_fwsec_sb_result(bar0.regs), OK);

    bar0.write(BOOT_PROGRESS_READ_PROTECTION, RTX_3080_READ_PROTECTION & ~READABLE_AT_LEVEL_0);
    EXPECT_EQ(check_fwsec_sb_result(bar0.regs), ERR_BAD_STATE);

    bar0.write(BOOT_PROGRESS_READ_PROTECTION, READABLE_AT_LEVEL_0);
    EXPECT_EQ(check_fwsec_sb_result(bar0.regs), OK);

    put_sb_result(bar0);
    bar0.write(BOOT_PROGRESS, BOOT_PROGRESS_COMPLETE - 1);
    EXPECT_EQ(check_fwsec_sb_result(bar0.regs), ERR_BAD_STATE);

    bar0.write(BOOT_PROGRESS, BOOT_PROGRESS_COMPLETE >> 1);
    EXPECT_EQ(check_fwsec_sb_result(bar0.regs), ERR_BAD_STATE);
}

// Bits 15 to 0 of the scratch register hold FWSEC's SB error code, and any code but 0 is refused
TEST(nvidia_protected_memory, nonzero_sb_error_code_is_refused) {
    fake_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    put_sb_result(bar0);

    bar0.write(SB_ERROR_SCRATCH, 0x1);
    EXPECT_EQ(check_fwsec_sb_result(bar0.regs), ERR_BAD_STATE);

    bar0.write(SB_ERROR_SCRATCH, 0x8000);
    EXPECT_EQ(check_fwsec_sb_result(bar0.regs), ERR_BAD_STATE);

    bar0.write(SB_ERROR_SCRATCH, 0xFFFF0000);
    EXPECT_EQ(check_fwsec_sb_result(bar0.regs), OK);
}

// GA102's values as NVIDIA's driver sizes the firmware's memory: 20 MiB of the heap for the firmware's operating
// system, an 8 MiB base share, a minimum heap of 84 MiB, a 1 MiB unprotected heap and a prescrubbed top of 256 MiB
static constexpr chip_gsp_reservation GA102_GSP_RESERVATION = {
    .heap_os_bytes = 0x1400000,
    .heap_base_bytes = 0x800000,
    .heap_min_bytes = 0x5400000,
    .unprotected_heap_bytes = 0x100000,
    .prescrubbed_top_bytes = 0x10000000,
};

// The sizes of the bootloader's program and the GSP's firmware in GA102's files
static constexpr uint64_t GA102_BOOTLOADER_BYTES = 24576;
static constexpr uint64_t GA102_FIRMWARE_BYTES   = 38141952;

// The heap GA102's values give for up to 10 GiB of video memory: 20 + 8 MiB, 1 MiB for the video memory and 96 MiB
// for the firmware's records of channels
static constexpr uint64_t GA102_HEAP_BYTES = 0x7D00000;

static constexpr uint64_t GIB = 0x40000000;

static int32_t lay_out_for_ga102(const frts_region& region, uint64_t firmware_bytes, protected_memory_layout& layout) {
    return lay_out_protected_memory(region, GA102_GSP_RESERVATION, GA102_BOOTLOADER_BYTES, firmware_bytes, layout);
}

// The RTX 3080 booted on its own framebuffer gets NVIDIA's layout for GA102's files below its FRTS region
TEST(nvidia_protected_memory, rtx_3080_protected_memory_layout) {
    protected_memory_layout layout = {};
    ASSERT_EQ(lay_out_for_ga102(RTX_3080_FRTS_REGION, GA102_FIRMWARE_BYTES, layout), OK);

    EXPECT_EQ(layout.bootloader_offset, 0x27FDFA000u);
    EXPECT_EQ(layout.firmware_offset, 0x27D990000u);
    EXPECT_EQ(layout.heap_offset, 0x275C00000u);
    EXPECT_EQ(layout.heap_bytes, GA102_HEAP_BYTES);
    EXPECT_EQ(layout.protected_start, 0x275B00000u);
    EXPECT_EQ(layout.unprotected_heap_offset, 0x275A00000u);
}

// With a valid workspace register the RTX 3080's region sits 896 KiB higher, the bootloader's program and the firmware
// move up 896 KiB with it, and the parts below them 1 MiB
TEST(nvidia_protected_memory, rtx_3080_layout_with_a_valid_workspace_register) {
    frts_region region = {
        .video_memory_size = 0x280000000,
        .vga_workspace_offset = 0x27FFE0000,
        .offset = 0x27FEE0000,
        .has_display = true,
    };

    protected_memory_layout layout = {};
    ASSERT_EQ(lay_out_for_ga102(region, GA102_FIRMWARE_BYTES, layout), OK);
    EXPECT_EQ(layout.bootloader_offset, 0x27FEDA000u);
    EXPECT_EQ(layout.firmware_offset, 0x27DA70000u);
    EXPECT_EQ(layout.heap_offset, 0x275D00000u);
    EXPECT_EQ(layout.heap_bytes, GA102_HEAP_BYTES);
    EXPECT_EQ(layout.protected_start, 0x275C00000u);
    EXPECT_EQ(layout.unprotected_heap_offset, 0x275B00000u);
}

// The bootloader's program or the firmware, one byte larger than the room from its boundary to the part above it,
// starts a whole boundary lower, 4 KiB lower for the program and 64 KiB lower for the firmware
TEST(nvidia_protected_memory, bootloader_and_firmware_start_on_their_boundaries) {
    uint64_t bootloader_bytes = GA102_BOOTLOADER_BYTES + 1;
    uint64_t firmware_bytes = 0x2469001;

    protected_memory_layout layout = {};
    ASSERT_EQ(
        lay_out_protected_memory(RTX_3080_FRTS_REGION, GA102_GSP_RESERVATION, bootloader_bytes, firmware_bytes, layout),
        OK
    );
    EXPECT_EQ(layout.bootloader_offset, 0x27FDF9000u);
    EXPECT_EQ(layout.firmware_offset, 0x27D980000u);
    EXPECT_EQ(layout.heap_offset, 0x275C00000u);
}

// The region below the last 1 MiB of `video_memory_size` bytes of video memory, as `place_frts_region` places it
// without a valid workspace register
static frts_region region_below_last_mib(uint64_t video_memory_size) {
    return {
        .video_memory_size = video_memory_size,
        .vga_workspace_offset = video_memory_size - VGA_WORKSPACE_MAX_BYTES,
        .offset = video_memory_size - VGA_WORKSPACE_MAX_BYTES - FRTS_REGION_BYTES,
        .has_display = true,
    };
}

// The heap GA102's files get with `video_memory_size` bytes of video memory, or 0 when the layout is refused
static uint64_t ga102_heap_bytes_for(uint64_t video_memory_size) {
    protected_memory_layout layout = {};
    if (lay_out_for_ga102(region_below_last_mib(video_memory_size), GA102_FIRMWARE_BYTES, layout) != OK) {
        return 0;
    }

    return layout.heap_bytes;
}

// The heap takes 96 KiB for each GiB of video memory, a part GiB counting whole and the product rounded up to 1 MiB
TEST(nvidia_protected_memory, heap_grows_with_video_memory) {
    EXPECT_EQ(ga102_heap_bytes_for(10 * GIB + MIB), GA102_HEAP_BYTES + MIB);
    EXPECT_EQ(ga102_heap_bytes_for(32 * GIB), GA102_HEAP_BYTES + 2 * MIB);
    EXPECT_EQ(ga102_heap_bytes_for(32 * GIB + MIB), GA102_HEAP_BYTES + 3 * MIB);
}

// The heap is summed in 32 bits as NVIDIA's driver sums it: 0x2AAAB and 0x15556 GiB wrap the product to 32 and 64 KiB,
// and 43679 GiB rounds it to 0xFFF00000, which wraps the sum to 123 MiB
TEST(nvidia_protected_memory, heap_is_sized_in_32_bits) {
    EXPECT_EQ(ga102_heap_bytes_for(0x2AAAB * GIB), GA102_HEAP_BYTES);
    EXPECT_EQ(ga102_heap_bytes_for(0x15556 * GIB), GA102_HEAP_BYTES);
    EXPECT_EQ(ga102_heap_bytes_for(43679 * GIB), GA102_HEAP_BYTES - 2 * MIB);
}

// The heap is raised to the chip's minimum: GA102's 84 MiB once 42368 GiB wraps the sum to 0, and a 200 MiB minimum
// above the RTX 3080's 125 MiB sum
TEST(nvidia_protected_memory, heap_is_raised_to_the_chip_minimum) {
    EXPECT_EQ(ga102_heap_bytes_for(42368 * GIB), GA102_GSP_RESERVATION.heap_min_bytes);

    chip_gsp_reservation reservation = GA102_GSP_RESERVATION;
    reservation.heap_min_bytes = 200 * MIB;

    protected_memory_layout layout = {};
    ASSERT_EQ(
        lay_out_protected_memory(
            RTX_3080_FRTS_REGION,
            reservation,
            GA102_BOOTLOADER_BYTES,
            GA102_FIRMWARE_BYTES,
            layout
        ),
        OK
    );
    EXPECT_EQ(layout.heap_bytes, 200 * MIB);
}

// The chip's values size the heaps and a prescrubbed top past 32 bits. A 116.5 MiB heap gets the 117 MiB of room
// it leaves below the firmware, and a 2.75 MiB unprotected heap starts 3 MiB below protected memory's start
TEST(nvidia_protected_memory, chip_values_size_the_heaps_and_the_prescrubbed_top) {
    chip_gsp_reservation reservation = {
        .heap_os_bytes = 0x1080000,
        .heap_base_bytes = 0x300000,
        .heap_min_bytes = 0x4000000,
        .unprotected_heap_bytes = 0x2C0000,
        .prescrubbed_top_bytes = 0x110000000,
    };

    uint64_t firmware_bytes = 0xFFBA000;

    protected_memory_layout layout = {};
    ASSERT_EQ(
        lay_out_protected_memory(RTX_3080_FRTS_REGION, reservation, GA102_BOOTLOADER_BYTES, firmware_bytes, layout),
        OK
    );
    EXPECT_EQ(layout.firmware_offset, 0x26FE40000u);
    EXPECT_EQ(layout.heap_offset, 0x268900000u);
    EXPECT_EQ(layout.heap_bytes, 0x7500000u);
    EXPECT_EQ(layout.protected_start, 0x268800000u);
    EXPECT_EQ(layout.unprotected_heap_offset, 0x268500000u);
    EXPECT_EQ(layout.unprotected_heap_bytes, 0x2C0000u);

    reservation.prescrubbed_top_bytes = 0x10000000;
    EXPECT_EQ(
        lay_out_protected_memory(RTX_3080_FRTS_REGION, reservation, GA102_BOOTLOADER_BYTES, firmware_bytes, layout),
        ERR_BAD_STATE
    );
}

// The largest firmware whose layout fits the RTX 3080's prescrubbed top puts the unprotected heap at its very start,
// one byte more is refused, leaving the layout as it was, and sizes 4 GiB larger than GA102's are refused too
TEST(nvidia_protected_memory, layout_must_fit_in_the_prescrubbed_top) {
    uint64_t largest_firmware_bytes = 0x7EFA000;
    uint64_t prescrubbed_start = RTX_3080_FRTS_REGION.video_memory_size - GA102_GSP_RESERVATION.prescrubbed_top_bytes;

    protected_memory_layout layout = {};
    ASSERT_EQ(lay_out_for_ga102(RTX_3080_FRTS_REGION, largest_firmware_bytes, layout), OK);
    EXPECT_EQ(layout.unprotected_heap_offset, prescrubbed_start);

    EXPECT_EQ(lay_out_for_ga102(RTX_3080_FRTS_REGION, largest_firmware_bytes + 1, layout), ERR_BAD_STATE);
    EXPECT_EQ(layout.firmware_offset, RTX_3080_FRTS_REGION.offset - GA102_BOOTLOADER_BYTES - largest_firmware_bytes);
    EXPECT_EQ(layout.unprotected_heap_offset, prescrubbed_start);

    uint64_t firmware_past_32_bits = (static_cast<uint64_t>(1) << 32) + GA102_FIRMWARE_BYTES;
    EXPECT_EQ(lay_out_for_ga102(RTX_3080_FRTS_REGION, firmware_past_32_bits, layout), ERR_BAD_STATE);

    uint64_t bootloader_past_32_bits = (static_cast<uint64_t>(1) << 32) + GA102_BOOTLOADER_BYTES;
    EXPECT_EQ(
        lay_out_protected_memory(
            RTX_3080_FRTS_REGION,
            GA102_GSP_RESERVATION,
            bootloader_past_32_bits,
            GA102_FIRMWARE_BYTES,
            layout
        ),
        ERR_BAD_STATE
    );
}

// Video memory smaller than the prescrubbed top must hold the layout from offset 0 up, and GA102's files need 166 MiB
TEST(nvidia_protected_memory, layout_must_fit_in_video_memory) {
    protected_memory_layout layout = {};
    ASSERT_EQ(lay_out_for_ga102(region_below_last_mib(166 * MIB), GA102_FIRMWARE_BYTES, layout), OK);
    EXPECT_EQ(layout.unprotected_heap_offset, 0u);

    EXPECT_EQ(lay_out_for_ga102(region_below_last_mib(165 * MIB), GA102_FIRMWARE_BYTES, layout), ERR_BAD_STATE);
}
