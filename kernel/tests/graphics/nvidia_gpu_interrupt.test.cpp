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
using drivers::nvidia::UNRESPONSIVE_CARD_SOURCES;

// The registers the handler reaches, as NVIDIA's headers give them: the CPU interrupt tree's leaves, the GSP falcon's
// interrupt status, masks and destinations, BAR0's mirror of NV_XVE_CYA_2 and the display's interrupt dispatch
static constexpr uint32_t NVIDIA_CPU_LEAF           = 0xB81000;
static constexpr uint32_t NVIDIA_GSP_IRQSTAT        = 0x110008;
static constexpr uint32_t NVIDIA_GSP_IRQMASK        = 0x110018;
static constexpr uint32_t NVIDIA_GSP_IRQDEST        = 0x11001C;
static constexpr uint32_t NVIDIA_GSP_RISCV_IRQMASK  = 0x111528;
static constexpr uint32_t NVIDIA_GSP_RISCV_IRQDEST  = 0x11152C;
static constexpr uint32_t NVIDIA_GSP_IRQSTAT_SWGEN0 = 1u << 6;
static constexpr uint32_t NVIDIA_XVE_CYA_2          = 0x88704;
static constexpr uint32_t NVIDIA_INTERRUPT_DISPATCH = 0x611EC0;

// The RTX 3080's GSP vector, bit 27 of leaf 4, and a display vector in another leaf, bit 6 of leaf 2
static constexpr interrupt_vectors CARD_VECTORS = {155, 70, true};
static constexpr uint32_t GSP_LEAF     = NVIDIA_CPU_LEAF + 4 * sizeof(uint32_t);
static constexpr uint32_t DISPLAY_LEAF = NVIDIA_CPU_LEAF + 2 * sizeof(uint32_t);
static constexpr uint32_t DISPLAY_BIT  = 1u << 6;

static constexpr uint32_t ALL_BITS_SET = 0xFFFFFFFF;

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

// Gives a driver with no device its BAR0 and vectors, turns on its display engine's flips and reaches the sources its
// handler kept for the task
struct nvidia_gpu_test_access {
    static nvidia_gpu_driver& new_driver(handler_test_bar0& bar0) {
        nvidia_gpu_driver& driver = *new (g_driver_bytes) nvidia_gpu_driver(nullptr);
        driver.m_bar_mappings[0].va = reinterpret_cast<uintptr_t>(bar0.bytes);
        driver.m_interrupt_vectors = CARD_VECTORS;
        return driver;
    }

    static uint32_t gsp_sources(const nvidia_gpu_driver& driver) {
        return driver.m_gsp_sources;
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

// A GSP source wakes the task and is kept for it, an interrupt without one does not wake it, and both re-arm MSI
TEST(nvidia_gpu_interrupt, handler_wakes_the_task_only_for_a_gsp_source) {
    handler_test_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    nvidia_gpu_driver& driver = nvidia_gpu_test_access::new_driver(bar0);
    route_swgen0_to_the_host(bar0);
    bar0.write(NVIDIA_GSP_IRQSTAT, NVIDIA_GSP_IRQSTAT_SWGEN0);
    bar0.write(NVIDIA_XVE_CYA_2, ALL_BITS_SET);

    EXPECT_TRUE(run_handler(driver));
    EXPECT_EQ(nvidia_gpu_test_access::gsp_sources(driver), NVIDIA_GSP_IRQSTAT_SWGEN0);
    EXPECT_EQ(bar0.read(NVIDIA_XVE_CYA_2), 0u);

    bar0.write(NVIDIA_GSP_IRQSTAT, 0);
    bar0.write(NVIDIA_XVE_CYA_2, ALL_BITS_SET);
    EXPECT_FALSE(run_handler(driver));
    EXPECT_EQ(nvidia_gpu_test_access::gsp_sources(driver), NVIDIA_GSP_IRQSTAT_SWGEN0);
    EXPECT_EQ(bar0.read(NVIDIA_XVE_CYA_2), 0u);
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

// Only a dispatch word or GSP registers reading all ones are a card that stopped responding: MSI is not re-armed,
// and after the GSP's all ones the display's vector is not read and the task wakes for the sources
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
    bar0.write(NVIDIA_GSP_IRQSTAT, ALL_BITS_SET);
    bar0.write(NVIDIA_GSP_IRQMASK, ALL_BITS_SET);
    bar0.write(NVIDIA_GSP_IRQDEST, ALL_BITS_SET);
    EXPECT_TRUE(run_handler(driver));

    EXPECT_EQ(nvidia_gpu_test_access::gsp_sources(driver), UNRESPONSIVE_CARD_SOURCES);
    EXPECT_EQ(bar0.read(DISPLAY_LEAF), ALL_BITS_SET);
    EXPECT_EQ(bar0.read(NVIDIA_XVE_CYA_2), ALL_BITS_SET);
    EXPECT_EQ(bar0.read(GSP_LEAF), 1u << 27);
}
