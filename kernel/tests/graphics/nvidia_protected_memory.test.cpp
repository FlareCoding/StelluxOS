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
using drivers::nvidia::ERR_BAD_STATE;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::frts_region;
using drivers::nvidia::OK;
using drivers::nvidia::place_frts_region;
using drivers::nvidia::registers;
using drivers::nvidia::wait_for_boot_firmware;

// Registers and fields the steps read, restated here so the tests hold the steps to NVIDIA's layout rather than
// to their own constants
static constexpr uint32_t BOOT_PROGRESS_READ_PROTECTION = 0x118128;
static constexpr uint32_t BOOT_PROGRESS                 = 0x118234;
static constexpr uint32_t GSP_FALCON_CPUCTL             = 0x110100;
static constexpr uint32_t PROTECTED_MEMORY_END          = 0x1FA828;
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
}

static constexpr uint32_t VIDEO_MEMORY_MIB = 64;
static constexpr uint64_t VIDEO_MEMORY_BYTES = VIDEO_MEMORY_MIB * MIB;

// The workspace register of a valid workspace starting at `start`, which must be a multiple of 64 KiB
static uint32_t vga_workspace_at(uint64_t start) {
    return (static_cast<uint32_t>(start / VGA_WORKSPACE_UNIT_BYTES) << VGA_WORKSPACE_START_SHIFT) | VGA_WORKSPACE_VALID;
}

// Without a display or a valid workspace register, the VGA workspace takes the last 1 MiB
TEST(nvidia_protected_memory, workspace_without_a_display_or_a_valid_register_takes_the_last_mib) {
    fake_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    uint32_t last_unit_register = vga_workspace_at(VIDEO_MEMORY_BYTES - VGA_WORKSPACE_UNIT_BYTES);

    put_layout(bar0, VIDEO_MEMORY_MIB, DISPLAY_DISABLED, last_unit_register);
    frts_region region = {};
    ASSERT_EQ(place_frts_region(bar0.regs, region), OK);
    EXPECT_EQ(region.vga_workspace_offset, VIDEO_MEMORY_BYTES - VGA_WORKSPACE_MAX_BYTES);
    EXPECT_EQ(region.offset, VIDEO_MEMORY_BYTES - VGA_WORKSPACE_MAX_BYTES - FRTS_REGION_BYTES);

    put_layout(bar0, VIDEO_MEMORY_MIB, 0, last_unit_register & ~VGA_WORKSPACE_VALID);
    region = {};
    ASSERT_EQ(place_frts_region(bar0.regs, region), OK);
    EXPECT_EQ(region.vga_workspace_offset, VIDEO_MEMORY_BYTES - VGA_WORKSPACE_MAX_BYTES);
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

    bar0.write(PROTECTED_MEMORY_END, 0x10);
    frts_region region = {.video_memory_size = 1, .vga_workspace_offset = 2, .offset = 3};
    EXPECT_EQ(place_frts_region(bar0.regs, region), ERR_BAD_STATE);
    EXPECT_EQ(region.video_memory_size, 1u);
    EXPECT_EQ(region.vga_workspace_offset, 2u);
    EXPECT_EQ(region.offset, 3u);

    bar0.write(PROTECTED_MEMORY_END, 0x80000000);
    EXPECT_EQ(place_frts_region(bar0.regs, region), ERR_BAD_STATE);

    bar0.write(PROTECTED_MEMORY_END, 0xF);
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
