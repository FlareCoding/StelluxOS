// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "clock/clock.h"
#include "drivers/graphics/nvidia/falcon.h"
#include "drivers/graphics/nvidia/firmware_files.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/registers.h"
#include "mm/heap.h"

TEST_SUITE(nvidia_falcon);

using drivers::nvidia::ERR_BAD_FIRMWARE;
using drivers::nvidia::ERR_BAD_STATE;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::FALCON_READY_TIMEOUT_NS;
using drivers::nvidia::FALCON_TIMEOUT_NS;
using drivers::nvidia::OK;
using drivers::nvidia::booter_layout;
using drivers::nvidia::booter_signature;
using drivers::nvidia::falcon_program;
using drivers::nvidia::has_gsp_riscv_core;
using drivers::nvidia::notify_gsp_of_queued_messages;
using drivers::nvidia::registers;
using drivers::nvidia::reset_gsp_falcon;
using drivers::nvidia::reset_gsp_into_riscv;
using drivers::nvidia::run_gsp_falcon_program;
using drivers::nvidia::select_booter_signature;
using drivers::nvidia::stop_gsp;
using drivers::nvidia::write_libos_arguments_address;

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

// The RTX 3080's GSP has a RISC-V core, and a GSP whose hardware configuration has every bit but the RISC-V bit set
// has none
TEST(nvidia_falcon, riscv_core_is_read_from_hardware_configuration) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    bar0.write(GSP_FALCON_HWCFG2, RTX_3080_HWCFG2);
    EXPECT_TRUE(has_gsp_riscv_core(bar0.regs));

    bar0.write(GSP_FALCON_HWCFG2, ~HAS_RISCV_CORE);
    EXPECT_FALSE(has_gsp_riscv_core(bar0.regs));
}

// Registers and values the reset into the RISC-V core and the steps after it use or leave alone, restated here so the
// tests hold them to NVIDIA's layout
static constexpr uint32_t GSP_FALCON_MAILBOX0 = 0x110040;
static constexpr uint32_t GSP_FALCON_MAILBOX1 = 0x110044;
static constexpr uint32_t GSP_QUEUE_HEAD0     = 0x110C00;
static constexpr uint32_t GSP_QUEUE_HEAD1     = 0x110C08;

static constexpr uint32_t SELECT_RISCV_CORE = 0x111; // the RISC-V core, the valid bit and BRFETCH

// What the RM register holds before a reset that must leave it alone, as only the full reset writes it
static constexpr uint32_t RM_BEFORE_RESET = 0x2468ACE1;

// With the RTX 3080's hardware configuration and boot control values and every engine bit set, the GSP is reset into
// its RISC-V core, releasing its engine reset with the register's other bits kept and leaving the RM register alone
TEST(nvidia_falcon, gsp_is_reset_into_riscv) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    bar0.write(GSP_FALCON_ENGINE, ~0u);
    bar0.write(GSP_FALCON_HWCFG2, RTX_3080_HWCFG2);
    bar0.write(GSP_RISCV_BCR_CTRL, RTX_3080_BCR);
    bar0.write(GSP_FALCON_RM, RM_BEFORE_RESET);

    EXPECT_EQ(reset_gsp_into_riscv(bar0.regs, 0, 0), OK);
    EXPECT_EQ(bar0.read(GSP_FALCON_ENGINE), ~ENGINE_RESET);
    EXPECT_EQ(bar0.read(GSP_RISCV_BCR_CTRL), SELECT_RISCV_CORE);
    EXPECT_EQ(bar0.read(GSP_FALCON_RM), RM_BEFORE_RESET);
}

// A GSP with its RISC-V core already selected is reset into it with no switch to its falcon core, which only the full
// reset makes and which would time out here, since the fake never confirms a switch
TEST(nvidia_falcon, reset_into_riscv_makes_no_core_switch) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    bar0.write(GSP_FALCON_HWCFG2, RTX_3080_HWCFG2);
    bar0.write(GSP_RISCV_BCR_CTRL, RISCV_CORE_SELECTED | CORE_SELECT_VALID);

    EXPECT_EQ(reset_gsp_into_riscv(bar0.regs, 0, SHORT_TIMEOUT_NS), OK);
    EXPECT_EQ(bar0.read(GSP_RISCV_BCR_CTRL), SELECT_RISCV_CORE);
}

// A falcon that never reports it is ready for a reset is reset into the RISC-V core once the ready timeout passes, and
// the boot control register, which starts with every bit set, is written whole with the selection
TEST(nvidia_falcon, reset_into_riscv_goes_on_without_ready_bit) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    bar0.write(GSP_FALCON_HWCFG2, HAS_RISCV_CORE);
    bar0.write(GSP_RISCV_BCR_CTRL, ~0u);

    uint64_t start = clock::now_ns();
    EXPECT_EQ(reset_gsp_into_riscv(bar0.regs, SHORT_TIMEOUT_NS, 0), OK);
    uint64_t elapsed = clock::now_ns() - start;

    EXPECT_GE(elapsed, SHORT_TIMEOUT_NS);
    EXPECT_LT(elapsed, RESET_TIME_LIMIT_NS);
    EXPECT_EQ(bar0.read(GSP_RISCV_BCR_CTRL), SELECT_RISCV_CORE);
}

// A falcon that keeps clearing its memories fails the reset into the RISC-V core once the timeout passes, with the
// boot control register left as it was
TEST(nvidia_falcon, reset_into_riscv_waits_for_cleared_memories) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    bar0.write(GSP_FALCON_HWCFG2, HAS_RISCV_CORE | CLEARING_MEMORIES | READY_FOR_RESET);
    bar0.write(GSP_RISCV_BCR_CTRL, RTX_3080_BCR);

    uint64_t start = clock::now_ns();
    EXPECT_EQ(reset_gsp_into_riscv(bar0.regs, 0, SHORT_TIMEOUT_NS), ERR_TIMEOUT);
    uint64_t elapsed = clock::now_ns() - start;

    EXPECT_GE(elapsed, SHORT_TIMEOUT_NS);
    EXPECT_LT(elapsed, RESET_TIME_LIMIT_NS);
    EXPECT_EQ(bar0.read(GSP_RISCV_BCR_CTRL), RTX_3080_BCR);
}

// The notice writes 0 to the resource manager's queue head register and leaves the next one alone
TEST(nvidia_falcon, queued_messages_notice_writes_resource_manager_queue_head) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    bar0.write(GSP_QUEUE_HEAD0, ~0u);
    bar0.write(GSP_QUEUE_HEAD1, ~0u);

    notify_gsp_of_queued_messages(bar0.regs);
    EXPECT_EQ(bar0.read(GSP_QUEUE_HEAD0), 0u);
    EXPECT_EQ(bar0.read(GSP_QUEUE_HEAD1), ~0u);
}

// An address with bits set past 16 bits in both halves goes low half into the first mailbox and high half into the
// second, and each mailbox, which starts with every bit set, is written whole
TEST(nvidia_falcon, libos_arguments_address_is_split_over_mailboxes) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    bar0.write(GSP_FALCON_MAILBOX0, ~0u);
    bar0.write(GSP_FALCON_MAILBOX1, ~0u);

    write_libos_arguments_address(bar0.regs, 0x876543219ABCD000);
    EXPECT_EQ(bar0.read(GSP_FALCON_MAILBOX0), 0x9ABCD000u);
    EXPECT_EQ(bar0.read(GSP_FALCON_MAILBOX1), 0x87654321u);
}

// Registers and fields loading and running a program use, restated here so the tests hold the run to NVIDIA's layout
static constexpr uint32_t GSP_FALCON_CPUCTL              = 0x110100;
static constexpr uint32_t GSP_FALCON_BOOTVEC             = 0x110104;
static constexpr uint32_t GSP_FALCON_DMACTL              = 0x11010C;
static constexpr uint32_t GSP_FALCON_DMATRFBASE          = 0x110110;
static constexpr uint32_t GSP_FALCON_DMATRFMOFFS         = 0x110114;
static constexpr uint32_t GSP_FALCON_DMATRFCMD           = 0x110118;
static constexpr uint32_t GSP_FALCON_DMATRFFBOFFS        = 0x11011C;
static constexpr uint32_t GSP_FALCON_DMATRFBASE1         = 0x110128;
static constexpr uint32_t GSP_FALCON_CPUCTL_ALIAS        = 0x110130;
static constexpr uint32_t GSP_FBIF_TRANSCFG0             = 0x110600;
static constexpr uint32_t GSP_FBIF_CTL                   = 0x110624;
static constexpr uint32_t GSP_FALCON2_MOD_SEL            = 0x111180;
static constexpr uint32_t GSP_FALCON2_BROM_CURR_UCODE_ID = 0x111198;
static constexpr uint32_t GSP_FALCON2_BROM_ENGIDMASK     = 0x11119C;
static constexpr uint32_t GSP_FALCON2_BROM_PARAADDR0     = 0x111210;

static constexpr uint32_t START_FALCON                     = 0x2;
static constexpr uint32_t FALCON_HALTED                    = 0x10;
static constexpr uint32_t STARTS_THROUGH_ALIAS             = 0x40;
static constexpr uint32_t REQUIRE_CONTEXT                  = 0x1;
static constexpr uint32_t ALLOW_PHYSICAL_WITHOUT_CONTEXT   = 0x80;
static constexpr uint32_t TRANSFER_TARGET_MASK             = 0x3;
static constexpr uint32_t TARGET_COHERENT_SYSTEM_MEMORY    = 0x1;
static constexpr uint32_t TARGET_NONCOHERENT_SYSTEM_MEMORY = 0x2;
static constexpr uint32_t PHYSICAL_ADDRESSES               = 0x4;
static constexpr uint32_t DMA_SECURE                       = 0x4;
static constexpr uint32_t DMA_TO_IMEM                      = 0x10;
static constexpr uint32_t DMA_256_BYTES                    = 0x600;
static constexpr uint32_t RSA_3072                         = 0x1;

// A program above the run's 512 TiB bound, so its address in 256-byte units has bits past the 9-bit second base
// register, with every field distinct, and IMEM and DMEM bases with bits on both sides of the 24-bit memory offset
static constexpr falcon_program DISTINCT_PROGRAM = {
    .dma_address = 0x24A1234567000,
    .is_cached = false,
    .code_size = 0x300,
    .imem_physical_base = 0xAB341100,
    .imem_virtual_base = 0x12000,
    .data_size = 0x180,
    .dmem_physical_base = 0xCD560400,
    .signature_offset = 0x125A4,
    .engine_mask = 0x480,
    .ucode_id = 0x89,
};

// Control bits of the memory interface that loading a program keeps
static constexpr uint32_t FBIF_CTL_OTHER_BITS = 0x5;

// Far longer than any run here takes, since every wait in these tests ends at once or after a timeout of at most 1 ms
static constexpr uint64_t RUN_TIME_LIMIT_NS = 100000000;

// With the DMA engine set to read physical addresses without looking in the CPU's caches, an uncached program's first
// code block goes to IMEM marked secure, read from the program less its virtual base, and the falcon is left unstarted
TEST(nvidia_falcon, code_block_is_copied_before_start) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    bar0.write(GSP_FBIF_CTL, FBIF_CTL_OTHER_BITS);
    bar0.write(GSP_FALCON_DMACTL, REQUIRE_CONTEXT);
    bar0.write(GSP_FBIF_TRANSCFG0, ~0u);
    bar0.write(GSP_FALCON_CPUCTL, FALCON_HALTED);

    uint64_t start = clock::now_ns();
    EXPECT_EQ(run_gsp_falcon_program(bar0.regs, DISTINCT_PROGRAM, SHORT_TIMEOUT_NS), ERR_TIMEOUT);
    uint64_t elapsed = clock::now_ns() - start;

    EXPECT_GE(elapsed, SHORT_TIMEOUT_NS);
    EXPECT_LT(elapsed, RUN_TIME_LIMIT_NS);
    EXPECT_EQ(bar0.read(GSP_FBIF_CTL), FBIF_CTL_OTHER_BITS | ALLOW_PHYSICAL_WITHOUT_CONTEXT);
    EXPECT_EQ(bar0.read(GSP_FALCON_DMACTL), 0u);
    EXPECT_EQ(
        bar0.read(GSP_FBIF_TRANSCFG0),
        ~TRANSFER_TARGET_MASK | TARGET_NONCOHERENT_SYSTEM_MEMORY | PHYSICAL_ADDRESSES
    );
    EXPECT_EQ(bar0.read(GSP_FALCON_DMATRFBASE), 0x12345550u);
    EXPECT_EQ(bar0.read(GSP_FALCON_DMATRFBASE1), 0x4Au);
    EXPECT_EQ(bar0.read(GSP_FALCON_DMATRFMOFFS), 0x341100u);
    EXPECT_EQ(bar0.read(GSP_FALCON_DMATRFFBOFFS), 0x12000u);
    EXPECT_EQ(bar0.read(GSP_FALCON_DMATRFCMD), DMA_256_BYTES | DMA_TO_IMEM | DMA_SECURE);
    EXPECT_EQ(bar0.read(GSP_FALCON2_BROM_PARAADDR0), 0u);
    EXPECT_EQ(bar0.read(GSP_FALCON_BOOTVEC), 0u);
    EXPECT_EQ(bar0.read(GSP_FALCON_CPUCTL), FALCON_HALTED);
}

// Without code, the data's first block goes to DMEM with neither the IMEM nor the secure field, read from the
// program's start at offset 0 whatever the virtual base
TEST(nvidia_falcon, data_block_is_copied_to_dmem) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    falcon_program program = DISTINCT_PROGRAM;
    program.code_size = 0;

    EXPECT_EQ(run_gsp_falcon_program(bar0.regs, program, 0), ERR_TIMEOUT);
    EXPECT_EQ(bar0.read(GSP_FALCON_DMATRFBASE), 0x12345670u);
    EXPECT_EQ(bar0.read(GSP_FALCON_DMATRFBASE1), 0x4Au);
    EXPECT_EQ(bar0.read(GSP_FALCON_DMATRFMOFFS), 0x560400u);
    EXPECT_EQ(bar0.read(GSP_FALCON_DMATRFFBOFFS), 0u);
    EXPECT_EQ(bar0.read(GSP_FALCON_DMATRFCMD), DMA_256_BYTES);
    EXPECT_EQ(bar0.read(GSP_FALCON_BOOTVEC), 0u);
}

// A cached program is read looking in the CPU's caches, and a falcon with its control register's alias enabled gets
// the boot ROM's parameters and the boot vector and is started through the alias, the run ending once it halts
TEST(nvidia_falcon, falcon_is_started_through_alias) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    bar0.write(GSP_FALCON_CPUCTL, STARTS_THROUGH_ALIAS | FALCON_HALTED);
    falcon_program program = DISTINCT_PROGRAM;
    program.is_cached = true;
    program.code_size = 0;
    program.data_size = 0;

    EXPECT_EQ(run_gsp_falcon_program(bar0.regs, program, 0), OK);
    EXPECT_EQ(bar0.read(GSP_FBIF_TRANSCFG0), TARGET_COHERENT_SYSTEM_MEMORY | PHYSICAL_ADDRESSES);
    EXPECT_EQ(bar0.read(GSP_FALCON_DMATRFCMD), 0u);
    EXPECT_EQ(bar0.read(GSP_FALCON2_BROM_PARAADDR0), 0x125A4u);
    EXPECT_EQ(bar0.read(GSP_FALCON2_BROM_ENGIDMASK), 0x480u);
    EXPECT_EQ(bar0.read(GSP_FALCON2_BROM_CURR_UCODE_ID), 0x89u);
    EXPECT_EQ(bar0.read(GSP_FALCON2_MOD_SEL), RSA_3072);
    EXPECT_EQ(bar0.read(GSP_FALCON_BOOTVEC), 0x12000u);
    EXPECT_EQ(bar0.read(GSP_FALCON_CPUCTL_ALIAS), START_FALCON);
    EXPECT_EQ(bar0.read(GSP_FALCON_CPUCTL), STARTS_THROUGH_ALIAS | FALCON_HALTED);
}

// A falcon without the alias, halted as the RTX 3080's is after its reset or with every other control bit set, is
// started through its control register and fails the run when it does not halt again
TEST(nvidia_falcon, run_waits_for_halt) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    bar0.write(GSP_FALCON_CPUCTL, FALCON_HALTED);
    falcon_program program = DISTINCT_PROGRAM;
    program.code_size = 0;
    program.data_size = 0;

    uint64_t start = clock::now_ns();
    EXPECT_EQ(run_gsp_falcon_program(bar0.regs, program, SHORT_TIMEOUT_NS), ERR_TIMEOUT);
    uint64_t elapsed = clock::now_ns() - start;

    EXPECT_GE(elapsed, SHORT_TIMEOUT_NS);
    EXPECT_LT(elapsed, RUN_TIME_LIMIT_NS);
    EXPECT_EQ(bar0.read(GSP_FALCON_CPUCTL), START_FALCON);
    EXPECT_EQ(bar0.read(GSP_FALCON_CPUCTL_ALIAS), 0u);

    bar0.write(GSP_FALCON_CPUCTL, ~STARTS_THROUGH_ALIAS);
    EXPECT_EQ(run_gsp_falcon_program(bar0.regs, program, 0), ERR_TIMEOUT);
    EXPECT_EQ(bar0.read(GSP_FALCON_CPUCTL), START_FALCON);
    EXPECT_EQ(bar0.read(GSP_FALCON_CPUCTL_ALIAS), 0u);
}

// A falcon with the alias and every control bit but the halted bit set is started through the alias, and fails the
// run when it does not halt
TEST(nvidia_falcon, run_through_alias_fails_without_halt) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    bar0.write(GSP_FALCON_CPUCTL, ~FALCON_HALTED);
    falcon_program program = DISTINCT_PROGRAM;
    program.code_size = 0;
    program.data_size = 0;

    EXPECT_EQ(run_gsp_falcon_program(bar0.regs, program, 0), ERR_TIMEOUT);
    EXPECT_EQ(bar0.read(GSP_FALCON_CPUCTL_ALIAS), START_FALCON);
    EXPECT_EQ(bar0.read(GSP_FALCON_CPUCTL), ~FALCON_HALTED);
}

// The RISC-V core's control register and its active field, which stopping the GSP reads after its reset, restated
// here so the tests hold the stop to NVIDIA's layout
static constexpr uint32_t GSP_RISCV_CPUCTL = 0x111388;
static constexpr uint32_t RISCV_ACTIVE     = 0x80;

// The falcon's and the RISC-V core's control registers as the RTX 3080's read after its GSP's reset, the falcon halted
// and the RISC-V core not active
static constexpr uint32_t RTX_3080_CPUCTL       = 0x10;
static constexpr uint32_t RTX_3080_RISCV_CPUCTL = 0x10;

// The RTX 3080's GSP reads as stopped after its reset, which writes the RM register, and so does a GSP whose falcon
// control register has every bit set and whose RISC-V control register has every bit but the active bit set
TEST(nvidia_falcon, rtx_3080_gsp_is_stopped) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    bar0.write(GSP_FALCON_HWCFG2, RTX_3080_HWCFG2);
    bar0.write(GSP_RISCV_BCR_CTRL, RTX_3080_BCR);
    bar0.write(GSP_FALCON_CPUCTL, RTX_3080_CPUCTL);
    bar0.write(GSP_RISCV_CPUCTL, RTX_3080_RISCV_CPUCTL);

    EXPECT_EQ(stop_gsp(bar0.regs, RTX_3080_BOOT_0, 0, 0), OK);
    EXPECT_EQ(bar0.read(GSP_FALCON_RM), RTX_3080_BOOT_0);

    bar0.write(GSP_FALCON_CPUCTL, ~0u);
    bar0.write(GSP_RISCV_CPUCTL, ~RISCV_ACTIVE);
    EXPECT_EQ(stop_gsp(bar0.regs, RTX_3080_BOOT_0, 0, 0), OK);
}

// A falcon with every control bit but the halted bit set does not read as stopped after the reset
TEST(nvidia_falcon, gsp_is_not_stopped_without_halted_falcon) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    bar0.write(GSP_FALCON_CPUCTL, ~FALCON_HALTED);

    EXPECT_EQ(stop_gsp(bar0.regs, RTX_3080_BOOT_0, 0, 0), ERR_BAD_STATE);
}

// A RISC-V core whose control register has every bit set reads as running, so the GSP is not stopped even with its
// falcon halted
TEST(nvidia_falcon, gsp_is_not_stopped_with_running_riscv_core) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    bar0.write(GSP_FALCON_HWCFG2, RTX_3080_HWCFG2);
    bar0.write(GSP_RISCV_BCR_CTRL, RTX_3080_BCR);
    bar0.write(GSP_FALCON_CPUCTL, RTX_3080_CPUCTL);
    bar0.write(GSP_RISCV_CPUCTL, ~0u);

    EXPECT_EQ(stop_gsp(bar0.regs, RTX_3080_BOOT_0, 0, 0), ERR_BAD_STATE);
}

// A reset whose memories never clear fails the stop once the memory timeout passes, not at the ready timeout of 0, even
// with the falcon halted and the RISC-V core not running
TEST(nvidia_falcon, gsp_stop_fails_when_its_reset_times_out) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    bar0.write(GSP_FALCON_HWCFG2, CLEARING_MEMORIES);
    bar0.write(GSP_FALCON_CPUCTL, RTX_3080_CPUCTL);

    uint64_t start = clock::now_ns();
    EXPECT_EQ(stop_gsp(bar0.regs, RTX_3080_BOOT_0, 0, SHORT_TIMEOUT_NS), ERR_TIMEOUT);
    uint64_t elapsed = clock::now_ns() - start;

    EXPECT_GE(elapsed, SHORT_TIMEOUT_NS);
    EXPECT_LT(elapsed, RESET_TIME_LIMIT_NS);
    EXPECT_EQ(bar0.read(GSP_FALCON_RM), 0u);
}

// A memory timeout the stop must never spend on a ready wait
static constexpr uint64_t LONG_MEMORY_TIMEOUT_NS = 1000000000;

// A falcon that never reports it is ready holds the stop for the short timeout of each of its two ready waits, not for
// the long timeout of the memory waits, which end at once
TEST(nvidia_falcon, gsp_stop_gives_each_ready_wait_the_ready_timeout) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    bar0.write(GSP_FALCON_CPUCTL, RTX_3080_CPUCTL);

    uint64_t start = clock::now_ns();
    EXPECT_EQ(stop_gsp(bar0.regs, RTX_3080_BOOT_0, SHORT_TIMEOUT_NS, LONG_MEMORY_TIMEOUT_NS), OK);
    uint64_t elapsed = clock::now_ns() - start;

    EXPECT_GE(elapsed, 2 * SHORT_TIMEOUT_NS);
    EXPECT_LT(elapsed, RESET_TIME_LIMIT_NS);
}

// NVIDIA's wait for the ready report, and its default timeout for a GPU in graphics mode rather than compute mode
static constexpr uint64_t NVIDIA_READY_TIMEOUT_NS   = 150000;
static constexpr uint64_t NVIDIA_DEFAULT_TIMEOUT_NS = 4000000000;

// Bring-up gives the ready report NVIDIA's 150 us, and each wait for the memories, the core selection, a block's
// load or a program's halt NVIDIA's default timeout
TEST(nvidia_falcon, falcon_timeouts_are_nvidias) {
    EXPECT_EQ(FALCON_READY_TIMEOUT_NS, NVIDIA_READY_TIMEOUT_NS);
    EXPECT_EQ(FALCON_TIMEOUT_NS, NVIDIA_DEFAULT_TIMEOUT_NS);
}

// The first fuse register recording a version of a program each falcon runs, and the bytes from one to the next,
// restated here so the tests hold the choice to NVIDIA's register map rather than to the driver's constants
static constexpr uint32_t GSP_UCODE_VERSION_FUSES   = 0x8241C0;
static constexpr uint32_t SEC2_UCODE_VERSION_FUSES  = 0x824140;
static constexpr uint32_t UCODE_VERSION_FUSE_STRIDE = 4;

// Writes the register that `ucode_id` names among the fuse registers from `first_fuse`
static void put_ucode_fuses(falcon_bar0& bar0, uint32_t first_fuse, uint32_t ucode_id, uint32_t fuses) {
    bar0.write(first_fuse + UCODE_VERSION_FUSE_STRIDE * ucode_id - UCODE_VERSION_FUSE_STRIDE, fuses);
}

// Booter load as `read_booter_layout` returns it for the GA102 file the nvidia-firmware package installs
static constexpr booter_layout GA102_BOOTER_LAYOUT = {
    .code_offset = 0x100,
    .code_size = 0x8300,
    .data_offset = 0x8400,
    .data_size = 0x6200,
    .signature_slot = 0x10,
    .signatures_offset = 0x3C,
    .signature_size = 384,
    .signature_count = 2,
    .ucode_id = 3,
    .engine_mask = 0x1,
};

// Writes `fuses` to SEC2's fuse register for `booter_load`'s ucode id, then chooses booter load's signature
static int32_t select_for_sec2_fuses(
    falcon_bar0& bar0,
    const booter_layout& booter_load,
    uint32_t fuses,
    booter_signature& signature
) {
    put_ucode_fuses(bar0, SEC2_UCODE_VERSION_FUSES, booter_load.ucode_id, fuses);
    return select_booter_signature(bar0.regs, booter_load, signature);
}

// Of the GA102 booter load's two signatures, fuse version 0 takes the second, version 1 the first and version 2 none.
// The GSP's register for the same ucode id holds version 1, so reading it instead fails the version 0 and 2 cases.
TEST(nvidia_falcon, ga102_booter_load_signature_follows_its_fuse_version) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    put_ucode_fuses(bar0, GSP_UCODE_VERSION_FUSES, GA102_BOOTER_LAYOUT.ucode_id, 0x1);

    booter_signature signature = {};
    EXPECT_EQ(select_for_sec2_fuses(bar0, GA102_BOOTER_LAYOUT, 0x0, signature), OK);
    EXPECT_EQ(signature.fuse_version, 0u);
    EXPECT_EQ(signature.index, 1u);

    EXPECT_EQ(select_for_sec2_fuses(bar0, GA102_BOOTER_LAYOUT, 0x1, signature), OK);
    EXPECT_EQ(signature.fuse_version, 1u);
    EXPECT_EQ(signature.index, 0u);

    signature = {.fuse_version = 7, .index = 7};
    EXPECT_EQ(select_for_sec2_fuses(bar0, GA102_BOOTER_LAYOUT, 0x2, signature), ERR_BAD_FIRMWARE);
    EXPECT_EQ(signature.fuse_version, 7u);
    EXPECT_EQ(signature.index, 7u);
}

// A lone signature is chosen whatever version the fuses record, even 32, the version of bit 31
TEST(nvidia_falcon, lone_booter_load_signature_is_chosen_even_at_fuse_version_32) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    booter_layout lone = GA102_BOOTER_LAYOUT;
    lone.signature_count = 1;
    booter_signature signature = {};
    EXPECT_EQ(select_for_sec2_fuses(bar0, lone, 0x80000000, signature), OK);
    EXPECT_EQ(signature.fuse_version, 32u);
    EXPECT_EQ(signature.index, 0u);
}

// With 0x10002 signatures the index needs 17 bits. A ucode id of 0x10003, which 8 or 16 bits would cut to 3, GA102's
// id, gives fuse version 0 rather than the version 1 the register for id 3 records.
TEST(nvidia_falcon, booter_load_signature_choice_keeps_bits_past_16) {
    falcon_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    booter_layout many = GA102_BOOTER_LAYOUT;
    many.signature_count = 0x10002;
    booter_signature signature = {};
    EXPECT_EQ(select_for_sec2_fuses(bar0, many, 0x1, signature), OK);
    EXPECT_EQ(signature.index, 0x10000u);

    booter_layout wide_id = GA102_BOOTER_LAYOUT;
    wide_id.ucode_id = 0x10003;
    put_ucode_fuses(bar0, SEC2_UCODE_VERSION_FUSES, GA102_BOOTER_LAYOUT.ucode_id, 0x1);
    EXPECT_EQ(select_booter_signature(bar0.regs, wide_id, signature), OK);
    EXPECT_EQ(signature.fuse_version, 0u);
    EXPECT_EQ(signature.index, 1u);
}
