// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "clock/clock.h"
#include "drivers/graphics/nvidia/falcon.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/registers.h"
#include "mm/heap.h"

TEST_SUITE(nvidia_falcon);

using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::FALCON_READY_TIMEOUT_NS;
using drivers::nvidia::FALCON_TIMEOUT_NS;
using drivers::nvidia::OK;
using drivers::nvidia::registers;
using drivers::nvidia::reset_gsp_falcon;

// Registers and fields the reset uses, restated here so the tests hold the reset to NVIDIA's layout rather than to
// its own constants
static constexpr uint32_t GSP_FALCON_ENGINE  = 0x1103C0;
static constexpr uint32_t GSP_FALCON_HWCFG2  = 0x1100F4;
static constexpr uint32_t GSP_FALCON_RM      = 0x110084;
static constexpr uint32_t GSP_RISCV_BCR_CTRL = 0x111668;

static constexpr uint32_t ENGINE_RESET        = 0x1;
static constexpr uint32_t HAS_RISCV_CORE      = 0x400;
static constexpr uint32_t CLEARING_MEMORIES   = 0x1000;
static constexpr uint32_t READY_FOR_RESET     = 0x80000000;
static constexpr uint32_t RISCV_CORE_SELECTED = 0x10;
static constexpr uint32_t CORE_SELECT_VALID   = 0x1;

static constexpr uint64_t SHORT_TIMEOUT_NS = 1000000;

// Far longer than any reset here takes, since every wait in these tests ends at once or after a timeout of at most
// 1 ms
static constexpr uint64_t RESET_TIME_LIMIT_NS = 100000000;

// A zeroed fake of BAR0, all 16 MiB of it
struct falcon_bar0 {
    static constexpr size_t BYTES = 0x1000000;

    uint8_t*  bytes = static_cast<uint8_t*>(heap::uzalloc(BYTES));
    registers regs{reinterpret_cast<uintptr_t>(bytes)};

    ~falcon_bar0() { heap::ufree(bytes); }

    uint32_t read(uint32_t offset) const { return *reinterpret_cast<uint32_t*>(bytes + offset); }
    void write(uint32_t offset, uint32_t value) { *reinterpret_cast<uint32_t*>(bytes + offset) = value; }
};

// The registers as an RTX 3080 reports them before and after its GSP's falcon is reset, with a RISC-V core that is
// not selected and a falcon that never reports it is ready for a reset, and the chip's `PMC_BOOT_0` value
static constexpr uint32_t RTX_3080_ENGINE = 0x0;
static constexpr uint32_t RTX_3080_HWCFG2 = 0x47F7;
static constexpr uint32_t RTX_3080_BCR    = 0x1;
static constexpr uint32_t RTX_3080_BOOT_0 = 0xB72000A1;

// The falcon of the RTX 3080's GSP is reset with its falcon core kept, and the registers are read even for timeouts
// of 0
TEST(nvidia_falcon, rtx_3080_falcon_is_reset) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    bar0.write(GSP_FALCON_ENGINE, RTX_3080_ENGINE);
    bar0.write(GSP_FALCON_HWCFG2, RTX_3080_HWCFG2);
    bar0.write(GSP_RISCV_BCR_CTRL, RTX_3080_BCR);

    EXPECT_EQ(reset_gsp_falcon(bar0.regs, RTX_3080_BOOT_0, 0, 0), OK);
    EXPECT_EQ(bar0.read(GSP_FALCON_ENGINE), RTX_3080_ENGINE);
    EXPECT_EQ(bar0.read(GSP_RISCV_BCR_CTRL), RTX_3080_BCR);
    EXPECT_EQ(bar0.read(GSP_FALCON_RM), RTX_3080_BOOT_0);
}

// The engine's reset bit ends clear, and the engine register's other bits are kept
TEST(nvidia_falcon, engine_reset_keeps_other_bits) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    bar0.write(GSP_FALCON_ENGINE, ~0u);

    EXPECT_EQ(reset_gsp_falcon(bar0.regs, RTX_3080_BOOT_0, 0, 0), OK);
    EXPECT_EQ(bar0.read(GSP_FALCON_ENGINE), ~ENGINE_RESET);
}

// A falcon that never reports it is ready for a reset, with every bit of its hardware configuration register set but
// the ready and memory clearing bits, is reset anyway after each of the two resets waits out the ready timeout
TEST(nvidia_falcon, reset_goes_on_without_ready_bit) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    bar0.write(GSP_FALCON_HWCFG2, ~READY_FOR_RESET & ~CLEARING_MEMORIES);

    uint64_t start = clock::now_ns();
    EXPECT_EQ(reset_gsp_falcon(bar0.regs, RTX_3080_BOOT_0, SHORT_TIMEOUT_NS, 0), OK);
    uint64_t elapsed = clock::now_ns() - start;

    EXPECT_GE(elapsed, 2 * SHORT_TIMEOUT_NS);
    EXPECT_LT(elapsed, RESET_TIME_LIMIT_NS);
    EXPECT_EQ(bar0.read(GSP_FALCON_RM), RTX_3080_BOOT_0);
}

static constexpr uint64_t LONG_READY_TIMEOUT_NS = 1000000000;

// The ready bit ends its wait long before the ready timeout passes
TEST(nvidia_falcon, ready_bit_ends_its_wait) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    bar0.write(GSP_FALCON_HWCFG2, READY_FOR_RESET);

    uint64_t start = clock::now_ns();
    EXPECT_EQ(reset_gsp_falcon(bar0.regs, RTX_3080_BOOT_0, LONG_READY_TIMEOUT_NS, 0), OK);
    uint64_t elapsed = clock::now_ns() - start;

    EXPECT_LT(elapsed, RESET_TIME_LIMIT_NS);
}

// A falcon that keeps clearing its memories fails the reset once the timeout passes, and only that bit holds it up
TEST(nvidia_falcon, reset_waits_for_cleared_memories) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    bar0.write(GSP_FALCON_HWCFG2, CLEARING_MEMORIES | READY_FOR_RESET);

    uint64_t start = clock::now_ns();
    EXPECT_EQ(reset_gsp_falcon(bar0.regs, RTX_3080_BOOT_0, 0, SHORT_TIMEOUT_NS), ERR_TIMEOUT);
    uint64_t elapsed = clock::now_ns() - start;

    EXPECT_GE(elapsed, SHORT_TIMEOUT_NS);
    EXPECT_LT(elapsed, RESET_TIME_LIMIT_NS);
    EXPECT_EQ(bar0.read(GSP_FALCON_RM), 0u);

    bar0.write(GSP_FALCON_HWCFG2, ~CLEARING_MEMORIES);
    EXPECT_EQ(reset_gsp_falcon(bar0.regs, RTX_3080_BOOT_0, 0, 0), OK);
}

// Each full reset releases the engine reset before it waits for the memories, and waits for them before it
// switches cores
TEST(nvidia_falcon, memory_wait_comes_between_engine_reset_and_core_switch) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    bar0.write(GSP_FALCON_ENGINE, ~0u);
    bar0.write(GSP_FALCON_HWCFG2, HAS_RISCV_CORE | CLEARING_MEMORIES | READY_FOR_RESET);
    bar0.write(GSP_RISCV_BCR_CTRL, RISCV_CORE_SELECTED | CORE_SELECT_VALID);

    EXPECT_EQ(reset_gsp_falcon(bar0.regs, RTX_3080_BOOT_0, 0, SHORT_TIMEOUT_NS), ERR_TIMEOUT);
    EXPECT_EQ(bar0.read(GSP_FALCON_ENGINE), ~ENGINE_RESET);
    EXPECT_EQ(bar0.read(GSP_RISCV_BCR_CTRL), RISCV_CORE_SELECTED | CORE_SELECT_VALID);
    EXPECT_EQ(bar0.read(GSP_FALCON_RM), 0u);
}

// A GSP with its RISC-V core selected is switched to its falcon core, and a switch that never takes effect fails
// the reset
TEST(nvidia_falcon, riscv_core_is_switched_and_waits_for_confirmation) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    bar0.write(GSP_FALCON_HWCFG2, HAS_RISCV_CORE | READY_FOR_RESET);
    bar0.write(GSP_RISCV_BCR_CTRL, RISCV_CORE_SELECTED | CORE_SELECT_VALID);

    uint64_t start = clock::now_ns();
    EXPECT_EQ(reset_gsp_falcon(bar0.regs, RTX_3080_BOOT_0, 0, SHORT_TIMEOUT_NS), ERR_TIMEOUT);
    uint64_t elapsed = clock::now_ns() - start;

    EXPECT_GE(elapsed, SHORT_TIMEOUT_NS);
    EXPECT_LT(elapsed, RESET_TIME_LIMIT_NS);
    EXPECT_EQ(bar0.read(GSP_RISCV_BCR_CTRL), 0u);
    EXPECT_EQ(bar0.read(GSP_FALCON_RM), 0u);
}

// The core is left alone when the GSP has no RISC-V core, even with its core select set, or has its falcon core
// selected, even with every other boot control bit set
TEST(nvidia_falcon, falcon_core_is_kept) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    bar0.write(GSP_FALCON_HWCFG2, ~HAS_RISCV_CORE & ~CLEARING_MEMORIES);
    bar0.write(GSP_RISCV_BCR_CTRL, RISCV_CORE_SELECTED | CORE_SELECT_VALID);
    EXPECT_EQ(reset_gsp_falcon(bar0.regs, RTX_3080_BOOT_0, 0, 0), OK);
    EXPECT_EQ(bar0.read(GSP_RISCV_BCR_CTRL), RISCV_CORE_SELECTED | CORE_SELECT_VALID);

    bar0.write(GSP_FALCON_HWCFG2, HAS_RISCV_CORE | READY_FOR_RESET);
    bar0.write(GSP_RISCV_BCR_CTRL, ~RISCV_CORE_SELECTED);
    EXPECT_EQ(reset_gsp_falcon(bar0.regs, RTX_3080_BOOT_0, 0, 0), OK);
    EXPECT_EQ(bar0.read(GSP_RISCV_BCR_CTRL), ~RISCV_CORE_SELECTED);
}

// NVIDIA's wait for the ready report, and its default timeout for a GPU in graphics mode rather than compute mode
static constexpr uint64_t NVIDIA_READY_TIMEOUT_NS   = 150000;
static constexpr uint64_t NVIDIA_DEFAULT_TIMEOUT_NS = 4000000000;

// Bring-up gives the ready report NVIDIA's 150 us, and each wait for the memories or the core selection NVIDIA's
// default timeout
TEST(nvidia_falcon, falcon_timeouts_are_nvidias) {
    EXPECT_EQ(FALCON_READY_TIMEOUT_NS, NVIDIA_READY_TIMEOUT_NS);
    EXPECT_EQ(FALCON_TIMEOUT_NS, NVIDIA_DEFAULT_TIMEOUT_NS);
}
