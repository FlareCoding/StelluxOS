// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/display_window_owners.h"
#include "drivers/graphics/nvidia/registers.h"

TEST_SUITE(nvidia_display_window_owners);

using drivers::nvidia::DISPLAY_WINDOWS_MAX;
using drivers::nvidia::read_window_owners;
using drivers::nvidia::registers;
using drivers::nvidia::WINDOW_OWNER_NONE;
using drivers::nvidia::window_owners;

// Where window 0's control lies in BAR0, NVIDIA's core channel area at 0x680000 with its armed state from 0x8000 and
// window 0's control at 0x1000 in it, restated so the tests hold the read to NVIDIA's layout
static constexpr uint32_t NVIDIA_BAR0_ARMED_WINDOW_CONTROL = 0x689000;

// The stride between two windows' controls, the owner field and its value naming no head, restated likewise
static constexpr uint32_t NVIDIA_WINDOW_STRIDE = 0x80;
static constexpr uint32_t NVIDIA_OWNER_MASK    = 0xF;
static constexpr uint8_t  NVIDIA_OWNER_NONE    = 0xF;

// The armed state's controls of every window, ordinary memory standing in for BAR0
static uint32_t g_armed_controls[DISPLAY_WINDOWS_MAX * NVIDIA_WINDOW_STRIDE / sizeof(uint32_t)];

// Sets window `window`'s control in the armed state to `control`
static void set_armed_control(uint8_t window, uint32_t control) {
    g_armed_controls[window * NVIDIA_WINDOW_STRIDE / sizeof(uint32_t)] = control;
}

// Reads the owners of the windows `present_windows` names, with BAR0 placing window 0's control at `g_armed_controls`
static window_owners read_owners(uint32_t present_windows) {
    registers bar0(reinterpret_cast<uintptr_t>(g_armed_controls) - NVIDIA_BAR0_ARMED_WINDOW_CONTROL);
    return read_window_owners(bar0, present_windows);
}

// The owner naming no head is NVIDIA's
TEST(nvidia_display_window_owners, owner_none_is_nvidias) {
    EXPECT_EQ(WINDOW_OWNER_NONE, NVIDIA_OWNER_NONE);
}

// Each present window takes the owner field of its control in the armed state, whatever its other bits hold, and the
// window after the last present one takes no owner
TEST(nvidia_display_window_owners, owners_come_from_each_windows_armed_control) {
    const uint32_t controls[] = {0x0, 0xFFFFFFF1, WINDOW_OWNER_NONE, 0x12, 0x3, 0xF0, 0xE, 0x7};
    for (uint8_t window = 0; window < 8; window++) {
        set_armed_control(window, controls[window]);
    }

    set_armed_control(8, 0x1);

    window_owners owners = read_owners(0xFF);

    for (uint8_t window = 0; window < 8; window++) {
        EXPECT_EQ(owners.owners[window], controls[window] & NVIDIA_OWNER_MASK);
    }

    EXPECT_EQ(owners.owners[8], 0u);
}

// Only the windows the present mask names take an owner, up to window 31, and every other window's entry stays 0
TEST(nvidia_display_window_owners, windows_not_present_take_no_owner) {
    for (uint8_t window = 0; window < DISPLAY_WINDOWS_MAX; window++) {
        set_armed_control(window, 0x2);
    }

    uint32_t present_windows = 0x80000005;
    window_owners owners = read_owners(present_windows);

    for (uint8_t window = 0; window < DISPLAY_WINDOWS_MAX; window++) {
        bool is_present = (present_windows & (1u << window)) != 0;
        EXPECT_EQ(owners.owners[window], is_present ? 2u : 0u);
    }
}
