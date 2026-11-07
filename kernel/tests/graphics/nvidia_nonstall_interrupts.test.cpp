// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#include "stlx_unit_test.h"
#include "common/logging.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/interrupts.h"
#include "drivers/graphics/nvidia/nvidia.h"

TEST_SUITE(nvidia_nonstall_interrupts);

using drivers::nvidia::acknowledge_nonstall_interrupt;
using drivers::nvidia::enable_vector;
using drivers::nvidia::ERR_BAD_MESSAGE;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::ERR_INVALID_ARG;
using drivers::nvidia::firmware;
using drivers::nvidia::mask_vector;
using drivers::nvidia::OK;
using drivers::nvidia::read_nonstall_vector;
using drivers::nvidia::registers;
namespace rm = drivers::nvidia::rm;

// NVIDIA's kernel interrupt table control, CE0's and GR0's engine numbers, the types of GR0, COPY2 and NVDEC0, and the
// CPU interrupt tree's registers, restated so the tests hold the driver to NVIDIA's values
static constexpr uint32_t GET_KERNEL_TABLE       = 0x20800A5C;
static constexpr uint16_t MC_ENGINE_IDX_CE0      = 15;
static constexpr uint16_t MC_ENGINE_IDX_GR0      = 82;
static constexpr uint32_t RM_ENGINE_TYPE_GR0     = 0x1;
static constexpr uint32_t RM_ENGINE_TYPE_COPY2   = 0xB;
static constexpr uint32_t RM_ENGINE_TYPE_NVDEC0  = 0x1D;
static constexpr uint32_t CPU_INTR_LEAF          = 0xB81000;
static constexpr uint32_t CPU_INTR_LEAF_EN_SET   = 0xB81200;
static constexpr uint32_t CPU_INTR_LEAF_EN_CLEAR = 0xB81400;
static constexpr uint32_t CPU_INTR_TOP_EN_SET    = 0xB81608;
static constexpr uint32_t CPU_INTR_TOP_EN_CLEAR  = 0xB81610;

// The RTX 3080's row for copy engine 2 (NVIDIA's COPY2) gives it nonstall vector 7, bit 7 of leaf 0
static constexpr uint32_t TEST_LEAF_COUNT = 8;
static constexpr uint16_t COPY2_ENGINE    = MC_ENGINE_IDX_CE0 + 2;
static constexpr uint32_t COPY2_VECTOR    = 7;
static constexpr uint32_t OUTSIDE_VECTOR  = TEST_LEAF_COUNT * 32;
static constexpr uint32_t STALE_WORD      = 0xA5A5A5A5;

// The RTX 3080's row for the graphics engine (NVIDIA's GR0) gives it nonstall vector 0, bit 0 of leaf 0
static constexpr uint32_t GR0_VECTOR = 0;

// A vector of leaf 2 and so of subtree 1, bit 6 of its leaf
static constexpr uint32_t HIGH_VECTOR      = 70;
static constexpr uint32_t HIGH_VECTOR_LEAF = 2;
static constexpr uint32_t HIGH_VECTOR_BIT  = 1u << 6;

// Answers the kernel interrupt table's internal control with its table and records the call
class interrupt_table_firmware : public firmware {
public:
    rm::kernel_interrupt_table_params table = {};
    int32_t                           result = OK;
    uint32_t                          control_count = 0;
    uint32_t                          internal_control_count = 0;
    uint32_t                          internal_command = 0;

    int32_t allocate(rm::handle, rm::handle, uint32_t, void*, size_t) override { return ERR_FIRMWARE; }

    int32_t control(rm::handle, uint32_t, void*, size_t) override {
        control_count++;
        return ERR_FIRMWARE;
    }

    int32_t release(rm::handle) override { return ERR_FIRMWARE; }

    rm::handle new_handle() override { return 0; }

    rm::handle client() const override { return 0; }

    int32_t internal_control(uint32_t command, void* params, size_t size) override {
        internal_control_count++;
        internal_command = command;
        if (result == OK && size == sizeof(table)) {
            string::memcpy(params, &table, sizeof(table));
        }

        return result;
    }

    void set_event_sink(drivers::nvidia::firmware_event_sink*) override {}

    void set_error_sink(drivers::nvidia::firmware_error_sink*) override {}

    void clear_rows() { string::memset(&table, 0, sizeof(table)); }

    void add_row(uint16_t engine, uint32_t pmc_interrupt_mask, uint32_t nonstall_vector) {
        table.rows[table.row_count] = {engine, pmc_interrupt_mask, 0, nonstall_vector};
        table.row_count++;
    }
};

// The CPU interrupt tree's registers, every word seeded with `STALE_WORD`
struct interrupt_tree_registers {
    static constexpr uint32_t WORDS = (CPU_INTR_TOP_EN_CLEAR - CPU_INTR_LEAF) / sizeof(uint32_t) + 1;

    uint32_t words[WORDS];

    interrupt_tree_registers() {
        for (uint32_t i = 0; i < WORDS; i++) {
            words[i] = STALE_WORD;
        }
    }

    registers view() { return registers(reinterpret_cast<uintptr_t>(words) - CPU_INTR_LEAF); }

    uint32_t& at(uint32_t offset) { return words[(offset - CPU_INTR_LEAF) / sizeof(uint32_t)]; }
};

// The copy engine's first row gives its nonstall vector, read through the internal client
TEST(nvidia_nonstall_interrupts, read_nonstall_vector_takes_the_engines_first_row) {
    interrupt_table_firmware fw;
    fw.add_row(MC_ENGINE_IDX_CE0, 0, 3);
    fw.add_row(COPY2_ENGINE, 0, COPY2_VECTOR);
    fw.add_row(COPY2_ENGINE, 0, COPY2_VECTOR + 1);

    uint32_t vector = 0;
    ASSERT_EQ(read_nonstall_vector(fw, RM_ENGINE_TYPE_COPY2, TEST_LEAF_COUNT, vector), OK);
    EXPECT_EQ(vector, COPY2_VECTOR);
    EXPECT_EQ(fw.internal_control_count, 1u);
    EXPECT_EQ(fw.internal_command, GET_KERNEL_TABLE);
    EXPECT_EQ(fw.control_count, 0u);
}

// The graphics engine's row gives its nonstall vector, and vector 0, the RTX 3080's, is read like any other
TEST(nvidia_nonstall_interrupts, read_nonstall_vector_takes_the_graphics_engines_row) {
    interrupt_table_firmware fw;
    fw.add_row(COPY2_ENGINE, 0, COPY2_VECTOR);
    fw.add_row(MC_ENGINE_IDX_GR0, 0, GR0_VECTOR);

    uint32_t vector = STALE_WORD;
    ASSERT_EQ(read_nonstall_vector(fw, RM_ENGINE_TYPE_GR0, TEST_LEAF_COUNT, vector), OK);
    EXPECT_EQ(vector, GR0_VECTOR);
    EXPECT_EQ(fw.internal_command, GET_KERNEL_TABLE);
}

static bool refuses_copy2(interrupt_table_firmware& fw) {
    uint32_t vector = STALE_WORD;
    int32_t rc = read_nonstall_vector(fw, RM_ENGINE_TYPE_COPY2, TEST_LEAF_COUNT, vector);
    return rc == ERR_BAD_MESSAGE && vector == STALE_WORD;
}

// A table without a usable row for the engine, or past `rm::KERNEL_INTERRUPT_ROWS_MAX` rows, is refused and leaves the
// vector unwritten
TEST(nvidia_nonstall_interrupts, read_nonstall_vector_refuses_tables_without_a_usable_row) {
    interrupt_table_firmware fw;
    fw.add_row(MC_ENGINE_IDX_CE0, 0, 3);
    EXPECT_TRUE(refuses_copy2(fw));

    fw.clear_rows();
    fw.add_row(COPY2_ENGINE, 0, OUTSIDE_VECTOR);
    EXPECT_TRUE(refuses_copy2(fw));

    fw.clear_rows();
    fw.add_row(COPY2_ENGINE, 1u << 5, COPY2_VECTOR);
    EXPECT_TRUE(refuses_copy2(fw));

    fw.clear_rows();
    fw.add_row(COPY2_ENGINE, 0, COPY2_VECTOR);
    fw.table.row_count = rm::KERNEL_INTERRUPT_ROWS_MAX + 1;
    EXPECT_TRUE(refuses_copy2(fw));
}

// The text logged while `INTERRUPTS_LOG_BACKEND` is set
static char     s_interrupts_log[512] = {};
static uint32_t s_interrupts_log_bytes = 0;

// Keeps each logged line's text, up to the buffer's end
static void keep_interrupts_log(const char* data, size_t len) {
    for (size_t i = 0; i < len && s_interrupts_log_bytes < sizeof(s_interrupts_log); i++) {
        s_interrupts_log[s_interrupts_log_bytes++] = data[i];
    }
}

static constexpr log::backend INTERRUPTS_LOG_BACKEND = {.write = keep_interrupts_log};

// Whether the kept text holds `text` anywhere
static bool interrupts_log_contains(const char* text) {
    size_t text_bytes = string::strlen(text);
    for (uint32_t start = 0; start + text_bytes <= s_interrupts_log_bytes; start++) {
        if (string::memcmp(s_interrupts_log + start, text, text_bytes) == 0) {
            return true;
        }
    }

    return false;
}

// A type of neither the graphics engine nor a copy engine is refused by a line naming it, without a control, and the
// control's error comes back as it is
TEST(nvidia_nonstall_interrupts, read_nonstall_vector_refuses_other_engines_and_returns_the_controls_error) {
    interrupt_table_firmware fw;
    fw.add_row(COPY2_ENGINE, 0, COPY2_VECTOR);

    uint32_t vector = STALE_WORD;
    s_interrupts_log_bytes = 0;
    log::set_backend(&INTERRUPTS_LOG_BACKEND);
    int32_t rc = read_nonstall_vector(fw, RM_ENGINE_TYPE_NVDEC0, TEST_LEAF_COUNT, vector);
    log::set_backend(nullptr);

    EXPECT_EQ(rc, ERR_INVALID_ARG);
    EXPECT_EQ(fw.internal_control_count, 0u);
    EXPECT_TRUE(interrupts_log_contains("engine type 0x1D names neither the graphics engine nor a copy engine"));

    fw.result = ERR_FIRMWARE;
    EXPECT_EQ(read_nonstall_vector(fw, RM_ENGINE_TYPE_COPY2, TEST_LEAF_COUNT, vector), ERR_FIRMWARE);
    EXPECT_EQ(vector, STALE_WORD);
}

// Enabling writes the vector's bit to its leaf's enable set and its subtree's bit to the top level's, and masking
// writes only the leaf's enable clear
TEST(nvidia_nonstall_interrupts, enable_and_mask_write_only_the_vectors_bits) {
    interrupt_tree_registers tree;
    registers regs = tree.view();

    enable_vector(regs, HIGH_VECTOR);
    EXPECT_EQ(tree.at(CPU_INTR_LEAF_EN_SET + HIGH_VECTOR_LEAF * 4), HIGH_VECTOR_BIT);
    EXPECT_EQ(tree.at(CPU_INTR_TOP_EN_SET), 1u << 1);
    EXPECT_EQ(tree.at(CPU_INTR_LEAF_EN_CLEAR + HIGH_VECTOR_LEAF * 4), STALE_WORD);

    mask_vector(regs, HIGH_VECTOR);
    EXPECT_EQ(tree.at(CPU_INTR_LEAF_EN_CLEAR + HIGH_VECTOR_LEAF * 4), HIGH_VECTOR_BIT);
    EXPECT_EQ(tree.at(CPU_INTR_TOP_EN_CLEAR), STALE_WORD);
}

// A pending vector is cleared by writing only its bit, and an idle one leaves its leaf unwritten
TEST(nvidia_nonstall_interrupts, acknowledge_clears_only_a_pending_vector) {
    interrupt_tree_registers tree;
    registers regs = tree.view();
    uint32_t& leaf = tree.at(CPU_INTR_LEAF + HIGH_VECTOR_LEAF * 4);

    leaf = HIGH_VECTOR_BIT | 1u;
    EXPECT_TRUE(acknowledge_nonstall_interrupt(regs, HIGH_VECTOR));
    EXPECT_EQ(leaf, HIGH_VECTOR_BIT);

    leaf = 1u;
    EXPECT_FALSE(acknowledge_nonstall_interrupt(regs, HIGH_VECTOR));
    EXPECT_EQ(leaf, 1u);
}
