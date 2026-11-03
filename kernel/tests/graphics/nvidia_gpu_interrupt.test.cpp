// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#include "stlx_unit_test.h"
#include "drivers/graphics/nvidia/display_vertical_blank.h"
#include "drivers/graphics/nvidia/interrupts.h"
#include "drivers/graphics/nvidia/nvidia_gpu.h"
#include "dynpriv/dynpriv.h"
#include "mm/heap.h"

TEST_SUITE(nvidia_gpu_interrupt);

using drivers::nvidia_gpu_driver;
using drivers::nvidia::interrupt_vectors;

// The registers the handler reaches or must leave alone, as NVIDIA's headers give them: CPU interrupt tree, GSP falcon
// interrupt and display dispatch registers and BAR0's mirror of NV_XVE_CYA_2
static constexpr uint32_t NVIDIA_CPU_LEAF           = 0xB81000;
static constexpr uint32_t NVIDIA_CPU_LEAF_EN_CLEAR  = 0xB81400;
static constexpr uint32_t NVIDIA_GSP_IRQSCLR        = 0x110004;
static constexpr uint32_t NVIDIA_GSP_IRQSTAT        = 0x110008;
static constexpr uint32_t NVIDIA_GSP_RISCV_IRQMASK  = 0x111528;
static constexpr uint32_t NVIDIA_GSP_RISCV_IRQDEST  = 0x11152C;
static constexpr uint32_t NVIDIA_GSP_INTR_RETRIGGER = 0x1103E8;
static constexpr uint32_t NVIDIA_GSP_IRQSTAT_SWGEN0 = 1u << 6;
static constexpr uint32_t NVIDIA_XVE_CYA_2          = 0x88704;
static constexpr uint32_t NVIDIA_INTERRUPT_DISPATCH = 0x611EC0;

// The RTX 3080's GSP vector, bit 27 of leaf 4, and a display vector in another leaf, bit 6 of leaf 2
static constexpr interrupt_vectors CARD_VECTORS = {155, 70, true};
static constexpr uint32_t GSP_LEAF              = NVIDIA_CPU_LEAF + 4 * sizeof(uint32_t);
static constexpr uint32_t GSP_LEAF_ENABLE_CLEAR = NVIDIA_CPU_LEAF_EN_CLEAR + 4 * sizeof(uint32_t);
static constexpr uint32_t GSP_BIT               = 1u << 27;
static constexpr uint32_t DISPLAY_LEAF          = NVIDIA_CPU_LEAF + 2 * sizeof(uint32_t);
static constexpr uint32_t DISPLAY_BIT           = 1u << 6;

static constexpr uint32_t ALL_BITS_SET = 0xFFFFFFFF;

// A register's seed, which shows whether the handler wrote the register
static constexpr uint32_t STALE_WORD = 0xA5A5A5A5;

// The BAR0 the driver under test maps, zeroed
struct handler_test_bar0 {
    static constexpr uint32_t BYTES = 0x1000000;

    uint8_t* bytes = static_cast<uint8_t*>(heap::uzalloc(BYTES));

    ~handler_test_bar0() { heap::ufree(bytes); }

    uint32_t read(uint32_t offset) const { return *reinterpret_cast<uint32_t*>(bytes + offset); }
    void write(uint32_t offset, uint32_t value) { *reinterpret_cast<uint32_t*>(bytes + offset) = value; }
};

// The driver under test, which its display engine makes too large for a test's stack
alignas(nvidia_gpu_driver) static uint8_t g_driver_bytes[sizeof(nvidia_gpu_driver)];

namespace drivers {

// Gives a driver with no device its BAR0 and vectors, turns on its display engine's flips and takes the GSP's service
// its handler left for the task, as the task does
struct nvidia_gpu_test_access {
    static nvidia_gpu_driver& new_driver(handler_test_bar0& bar0) {
        nvidia_gpu_driver& driver = *new (g_driver_bytes) nvidia_gpu_driver(nullptr);
        driver.m_bar_mappings[0].va = reinterpret_cast<uintptr_t>(bar0.bytes);
        driver.m_interrupt_vectors = CARD_VECTORS;
        return driver;
    }

    static void set_vectors(nvidia_gpu_driver& driver, const interrupt_vectors& vectors) {
        driver.m_interrupt_vectors = vectors;
    }

    static bool take_gsp_service(nvidia_gpu_driver& driver) {
        bool must_service_gsp = driver.m_must_service_gsp;
        driver.m_must_service_gsp = false;
        return must_service_gsp;
    }

    static void turn_on_flips(nvidia_gpu_driver& driver) {
        driver.m_display_engine.turn_on_flips();
    }
};

} // namespace drivers

using drivers::nvidia_gpu_test_access;

// Runs the handler elevated, as the card's interrupt entry does
static bool run_handler(nvidia_gpu_driver& driver) {
    bool has_work = false;
    RUN_ELEVATED(has_work = driver.on_interrupt(0));
    return has_work;
}

// Routes SWGEN0 to the host through the RISC-V core's mask and destination, as on the RTX 3080
static void route_swgen0_to_the_host(handler_test_bar0& bar0) {
    bar0.write(NVIDIA_GSP_RISCV_IRQMASK, NVIDIA_GSP_IRQSTAT_SWGEN0);
    bar0.write(NVIDIA_GSP_RISCV_IRQDEST, NVIDIA_GSP_IRQSTAT_SWGEN0);
}

// The handler goes by the GSP's leaf alone: a pending bit is taken for the task with no source pending in the falcon,
// idle runs keep that take and add none with SWGEN0 pending there. Each run re-arms MSI and writes no falcon register
TEST(nvidia_gpu_interrupt, handler_takes_the_gsp_vector_by_its_leaf_alone) {
    handler_test_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    nvidia_gpu_driver& driver = nvidia_gpu_test_access::new_driver(bar0);
    bar0.write(NVIDIA_GSP_IRQSCLR, STALE_WORD);
    bar0.write(NVIDIA_GSP_INTR_RETRIGGER, STALE_WORD);

    bar0.write(GSP_LEAF, GSP_BIT);
    bar0.write(GSP_LEAF_ENABLE_CLEAR, STALE_WORD);
    bar0.write(NVIDIA_XVE_CYA_2, ALL_BITS_SET);
    EXPECT_TRUE(run_handler(driver));

    EXPECT_EQ(bar0.read(GSP_LEAF_ENABLE_CLEAR), GSP_BIT);
    EXPECT_EQ(bar0.read(GSP_LEAF), GSP_BIT);
    EXPECT_EQ(bar0.read(NVIDIA_XVE_CYA_2), 0u);

    route_swgen0_to_the_host(bar0);
    bar0.write(NVIDIA_GSP_IRQSTAT, NVIDIA_GSP_IRQSTAT_SWGEN0);
    bar0.write(GSP_LEAF, ~GSP_BIT);
    bar0.write(GSP_LEAF_ENABLE_CLEAR, STALE_WORD);
    bar0.write(NVIDIA_XVE_CYA_2, ALL_BITS_SET);
    EXPECT_FALSE(run_handler(driver));

    EXPECT_TRUE(nvidia_gpu_test_access::take_gsp_service(driver));
    EXPECT_EQ(bar0.read(GSP_LEAF), ~GSP_BIT);
    EXPECT_EQ(bar0.read(GSP_LEAF_ENABLE_CLEAR), STALE_WORD);
    EXPECT_EQ(bar0.read(NVIDIA_XVE_CYA_2), 0u);

    bar0.write(NVIDIA_XVE_CYA_2, ALL_BITS_SET);
    EXPECT_FALSE(run_handler(driver));

    EXPECT_FALSE(nvidia_gpu_test_access::take_gsp_service(driver));
    EXPECT_EQ(bar0.read(NVIDIA_XVE_CYA_2), 0u);

    EXPECT_EQ(bar0.read(NVIDIA_GSP_IRQSCLR), STALE_WORD);
    EXPECT_EQ(bar0.read(NVIDIA_GSP_INTR_RETRIGGER), STALE_WORD);
}

// With the RTX 3080's vectors, the display's bit 26 beside the GSP's bit 27 in leaf 4, taking the GSP's vector disables
// its bit alone, so the display's vector stays enabled for the task's run
TEST(nvidia_gpu_interrupt, handler_keeps_the_display_vector_enabled_in_the_gsp_leaf) {
    handler_test_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    nvidia_gpu_driver& driver = nvidia_gpu_test_access::new_driver(bar0);

    nvidia_gpu_test_access::set_vectors(driver, {155, 154, true});
    nvidia_gpu_test_access::turn_on_flips(driver);
    bar0.write(GSP_LEAF, GSP_BIT | (1u << 26));
    bar0.write(GSP_LEAF_ENABLE_CLEAR, STALE_WORD);

    EXPECT_TRUE(run_handler(driver));
    EXPECT_EQ(bar0.read(GSP_LEAF_ENABLE_CLEAR), GSP_BIT);
}

// With flips off the display's vector stays pending and unread, with them on its bit alone is cleared, and a leaf
// without its bit leaves the dispatch word unread
TEST(nvidia_gpu_interrupt, handler_clears_the_display_vector_only_with_flips_on) {
    handler_test_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    nvidia_gpu_driver& driver = nvidia_gpu_test_access::new_driver(bar0);
    bar0.write(DISPLAY_LEAF, ALL_BITS_SET);

    EXPECT_FALSE(run_handler(driver));
    EXPECT_EQ(bar0.read(DISPLAY_LEAF), ALL_BITS_SET);

    nvidia_gpu_test_access::turn_on_flips(driver);
    bar0.write(NVIDIA_XVE_CYA_2, ALL_BITS_SET);
    EXPECT_FALSE(run_handler(driver));
    EXPECT_EQ(bar0.read(DISPLAY_LEAF), DISPLAY_BIT);
    EXPECT_EQ(bar0.read(NVIDIA_XVE_CYA_2), 0u);

    bar0.write(DISPLAY_LEAF, ~DISPLAY_BIT);
    bar0.write(NVIDIA_INTERRUPT_DISPATCH, ALL_BITS_SET);
    bar0.write(NVIDIA_XVE_CYA_2, ALL_BITS_SET);
    EXPECT_FALSE(run_handler(driver));
    EXPECT_EQ(bar0.read(DISPLAY_LEAF), ~DISPLAY_BIT);
    EXPECT_EQ(bar0.read(NVIDIA_XVE_CYA_2), 0u);
}

// Only a dispatch word or the GSP's leaf reading all ones is a card that stopped responding: MSI is not re-armed, and
// after the leaf's all ones the display's vector is not read, the leaf is left unwritten and the task wakes to report
TEST(nvidia_gpu_interrupt, handler_skips_the_rearm_for_a_card_that_stopped_responding) {
    handler_test_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    nvidia_gpu_driver& driver = nvidia_gpu_test_access::new_driver(bar0);
    nvidia_gpu_test_access::turn_on_flips(driver);

    uint32_t responding_dispatches[] = {0xFFFFFFFE, 0x7FFFFFFF};
    for (uint32_t dispatch : responding_dispatches) {
        bar0.write(DISPLAY_LEAF, ALL_BITS_SET);
        bar0.write(NVIDIA_INTERRUPT_DISPATCH, dispatch);
        bar0.write(NVIDIA_XVE_CYA_2, ALL_BITS_SET);

        EXPECT_FALSE(run_handler(driver));
        EXPECT_EQ(bar0.read(NVIDIA_XVE_CYA_2), 0u);
    }

    bar0.write(DISPLAY_LEAF, ALL_BITS_SET);
    bar0.write(NVIDIA_INTERRUPT_DISPATCH, ALL_BITS_SET);
    bar0.write(NVIDIA_XVE_CYA_2, ALL_BITS_SET);
    EXPECT_FALSE(run_handler(driver));
    EXPECT_EQ(bar0.read(NVIDIA_XVE_CYA_2), ALL_BITS_SET);

    bar0.write(DISPLAY_LEAF, ALL_BITS_SET);
    bar0.write(GSP_LEAF, ALL_BITS_SET);
    bar0.write(GSP_LEAF_ENABLE_CLEAR, STALE_WORD);
    EXPECT_TRUE(run_handler(driver));
    EXPECT_TRUE(nvidia_gpu_test_access::take_gsp_service(driver));

    EXPECT_EQ(bar0.read(DISPLAY_LEAF), ALL_BITS_SET);
    EXPECT_EQ(bar0.read(NVIDIA_XVE_CYA_2), ALL_BITS_SET);
    EXPECT_EQ(bar0.read(GSP_LEAF), ALL_BITS_SET);
    EXPECT_EQ(bar0.read(GSP_LEAF_ENABLE_CLEAR), STALE_WORD);
}
