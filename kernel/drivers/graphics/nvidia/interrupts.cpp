// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/interrupts.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/falcon.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "common/logging.h"
#include "mm/heap.h"

namespace drivers::nvidia {

// The CPU interrupt tree's registers (NVIDIA's NV_VIRTUAL_FUNCTION_PRIV_CPU_INTR_*, in the block at 0xB80000). A bit
// written to a leaf clears that pending bit, and bits written to an enable's set or clear register set or clear them
constexpr uint32_t CPU_INTERRUPT_LEAF              = 0xB81000;
constexpr uint32_t CPU_INTERRUPT_LEAF_ENABLE_SET   = 0xB81200;
constexpr uint32_t CPU_INTERRUPT_LEAF_ENABLE_CLEAR = 0xB81400;
constexpr uint32_t CPU_INTERRUPT_TOP_ENABLE_SET    = 0xB81608;
constexpr uint32_t CPU_INTERRUPT_TOP_ENABLE_CLEAR  = 0xB81610;

// The bytes from one leaf's register to the next in the leaves and in their enables' set and clear registers
constexpr uint32_t LEAF_REGISTER_BYTES = 4;

// The value written to an enable's clear register to clear every enable bit at once
constexpr uint32_t EVERY_BIT = 0xFFFFFFFF;

// What a leaf and the GSP falcon's pending sources read on a card that has stopped responding
constexpr uint32_t UNRESPONSIVE_CARD_LEAF    = 0xFFFFFFFF;
constexpr uint32_t UNRESPONSIVE_CARD_SOURCES = 0xFFFFFFFF;

// The GSP falcon's interrupt registers: its pending sources, their clear, each core's mask and destination, which pass
// a source to the host (the CPU), and the retrigger, which sends the falcon's interrupt again if a source is pending
constexpr uint32_t GSP_IRQ_CLEAR             = GSP_FALCON_BASE + 0x004;
constexpr uint32_t GSP_IRQ_STATUS            = GSP_FALCON_BASE + 0x008;
constexpr uint32_t GSP_IRQ_MASK              = GSP_FALCON_BASE + 0x018;
constexpr uint32_t GSP_IRQ_DESTINATION       = GSP_FALCON_BASE + 0x01C;
constexpr uint32_t GSP_INTERRUPT_RETRIGGER   = GSP_FALCON_BASE + 0x3E8;
constexpr uint32_t GSP_RISCV_IRQ_MASK        = GSP_FALCON_SECOND_BASE + 0x528;
constexpr uint32_t GSP_RISCV_IRQ_DESTINATION = GSP_FALCON_SECOND_BASE + 0x52C;

// The retrigger register's trigger bit
constexpr uint32_t TRIGGER_BIT = 1;

// The register whose write re-arms the card's MSI (NVIDIA's NV_XVE_CYA_2), and the value written, which the card
// ignores
constexpr uint32_t MSI_REARM       = PCI_CONFIG_MIRROR_OFFSET + 0x704;
constexpr uint32_t MSI_REARM_VALUE = 0;

// The first of the table's counted rows for `engine`, or null when it has none
static const rm::kernel_interrupt_row* find_first_row(const rm::kernel_interrupt_table_params& table, uint16_t engine) {
    for (uint32_t i = 0; i < table.row_count; i++) {
        if (table.rows[i].engine == engine) {
            return &table.rows[i];
        }
    }

    return nullptr;
}

// Finds the GSP's vector in the table the reply filled, whose count must already be checked
static int32_t find_gsp_vector(const rm::kernel_interrupt_table_params& table, uint32_t leaf_count, uint32_t& vector) {
    const rm::kernel_interrupt_row* row = find_first_row(table, rm::ENGINE_GSP);
    if (row == nullptr) {
        log::error(
            "nvidia: the GSP firmware's kernel interrupt table counts %u rows, none for the GSP",
            table.row_count
        );
        return ERR_BAD_MESSAGE;
    }

    uint32_t vector_count = leaf_count * VECTORS_PER_LEAF;
    if (row->stall_vector >= vector_count) {
        log::error(
            "nvidia: the GSP firmware's kernel interrupt table gives the GSP vector %u, outside the %u vectors "
            "of the CPU interrupt tree",
            row->stall_vector,
            vector_count
        );
        return ERR_BAD_MESSAGE;
    }

    if (row->pmc_interrupt_mask != 0) {
        log::error(
            "nvidia: the GSP firmware's kernel interrupt table gives the GSP's row the PMC mask 0x%X, and "
            "NVIDIA's driver never enables the vector of a row with one",
            row->pmc_interrupt_mask
        );
        return ERR_BAD_MESSAGE;
    }

    vector = row->stall_vector;
    return OK;
}

// Finds the display's vector in the table the reply filled, logging why the display has none
static bool find_display_vector(const rm::kernel_interrupt_table_params& table, uint32_t leaf_count, uint32_t& vector) {
    const rm::kernel_interrupt_row* row = find_first_row(table, rm::ENGINE_DISPLAY);
    if (row == nullptr) {
        log::warn(
            "nvidia: the GSP firmware's kernel interrupt table counts %u rows, none for the display, so the heads' "
            "vertical blanks are not serviced",
            table.row_count
        );
        return false;
    }

    uint32_t vector_count = leaf_count * VECTORS_PER_LEAF;
    if (row->stall_vector >= vector_count) {
        log::warn(
            "nvidia: the GSP firmware's kernel interrupt table gives the display vector %u, outside the %u vectors "
            "of the CPU interrupt tree, so the heads' vertical blanks are not serviced",
            row->stall_vector,
            vector_count
        );
        return false;
    }

    if (row->pmc_interrupt_mask != 0) {
        log::warn(
            "nvidia: the GSP firmware's kernel interrupt table gives the display's row the PMC mask 0x%X, so the "
            "heads' vertical blanks are not serviced",
            row->pmc_interrupt_mask
        );
        return false;
    }

    vector = row->stall_vector;
    return true;
}

// Sets `engine` to the number among the engines of the engine of type `engine_type`, returning false when that is
// neither the graphics engine nor a copy engine
static bool find_engine_number(uint32_t engine_type, uint16_t& engine) {
    if (engine_type == rm::ENGINE_TYPE_GRAPHICS) {
        engine = rm::ENGINE_GRAPHICS;
        return true;
    }

    bool is_copy_engine = engine_type >= rm::ENGINE_TYPE_FIRST_COPY && engine_type <= rm::ENGINE_TYPE_LAST_COPY;
    if (!is_copy_engine) {
        return false;
    }

    engine = static_cast<uint16_t>(rm::ENGINE_FIRST_COPY + engine_type - rm::ENGINE_TYPE_FIRST_COPY);
    return true;
}

// Finds the nonstall vector of the engine of type `engine_type`, numbered `engine`, in the table the reply filled,
// whose count must already be checked
static int32_t find_nonstall_vector(
    const rm::kernel_interrupt_table_params& table,
    uint32_t engine_type,
    uint16_t engine,
    uint32_t leaf_count,
    uint32_t& vector
) {
    const rm::kernel_interrupt_row* row = find_first_row(table, engine);
    if (row == nullptr) {
        log::error(
            "nvidia: the GSP firmware's kernel interrupt table counts %u rows, none for engine type 0x%X",
            table.row_count,
            engine_type
        );
        return ERR_BAD_MESSAGE;
    }

    uint32_t vector_count = leaf_count * VECTORS_PER_LEAF;
    if (row->nonstall_vector >= vector_count) {
        log::error(
            "nvidia: the GSP firmware's kernel interrupt table gives engine type 0x%X the nonstall vector %u, "
            "outside the %u vectors of the CPU interrupt tree",
            engine_type,
            row->nonstall_vector,
            vector_count
        );
        return ERR_BAD_MESSAGE;
    }

    if (row->pmc_interrupt_mask != 0) {
        log::error(
            "nvidia: the GSP firmware's kernel interrupt table gives engine type 0x%X's row the PMC mask 0x%X, and "
            "NVIDIA's driver never enables the vector of a row with one",
            engine_type,
            row->pmc_interrupt_mask
        );
        return ERR_BAD_MESSAGE;
    }

    vector = row->nonstall_vector;
    return OK;
}

static int32_t check_row_count(const rm::kernel_interrupt_table_params& table) {
    if (table.row_count > rm::KERNEL_INTERRUPT_ROWS_MAX) {
        log::error(
            "nvidia: the GSP firmware's kernel interrupt table counts %u rows, more than the %u it has room for",
            table.row_count,
            rm::KERNEL_INTERRUPT_ROWS_MAX
        );
        return ERR_BAD_MESSAGE;
    }

    return OK;
}

static uint32_t leaf_of(uint32_t vector) {
    return vector / VECTORS_PER_LEAF;
}

static uint32_t leaf_register(uint32_t first_leaf_register, uint32_t leaf) {
    return first_leaf_register + leaf * LEAF_REGISTER_BYTES;
}

static uint32_t vector_bit(uint32_t vector) {
    return 1u << (vector % VECTORS_PER_LEAF);
}

static_assert(
    INTERRUPT_LEAVES_MAX <= 32 * LEAVES_PER_SUBTREE,
    "the subtrees of a chip's leaves outgrow the 32 bits of the top level's first register"
);

// Every subtree's bit lies in the top level's first register, the only one the enable and the disable write, as no chip
// in the chip table has more than `INTERRUPT_LEAVES_MAX` leaves, whose subtrees fit in its 32 bits
static uint32_t subtree_bit(uint32_t vector) {
    return 1u << (leaf_of(vector) / LEAVES_PER_SUBTREE);
}

static void clear_leaf_enables(registers& regs, uint32_t leaf_count) {
    for (uint32_t leaf = 0; leaf < leaf_count; leaf++) {
        regs.write(leaf_register(CPU_INTERRUPT_LEAF_ENABLE_CLEAR, leaf), EVERY_BIT);
    }
}

// Reads the GSP's vector and, when `is_display_enabled`, the display's from the table the reply filled
static int32_t find_vectors(
    const rm::kernel_interrupt_table_params& table,
    uint32_t leaf_count,
    bool is_display_enabled,
    interrupt_vectors& vectors
) {
    int32_t rc = check_row_count(table);
    if (rc != OK) {
        return rc;
    }

    interrupt_vectors found = {};
    rc = find_gsp_vector(table, leaf_count, found.gsp);
    if (rc != OK) {
        return rc;
    }

    found.has_display = is_display_enabled && find_display_vector(table, leaf_count, found.display);
    vectors = found;
    return OK;
}

int32_t read_interrupt_vectors(
    gsp& conversation,
    rm::handle client,
    rm::handle subdevice,
    uint32_t leaf_count,
    bool is_display_enabled,
    uint64_t timeout_ns,
    interrupt_vectors& vectors
) {
    uint32_t table_bytes = sizeof(rm::kernel_interrupt_table_params);
    auto* table = static_cast<rm::kernel_interrupt_table_params*>(heap::uzalloc(table_bytes));
    if (table == nullptr) {
        log::error("nvidia: allocating %u bytes for the GSP firmware's kernel interrupt table failed", table_bytes);
        return ERR_NO_MEMORY;
    }

    int32_t rc = conversation.control(
        client,
        subdevice,
        rm::CONTROL_GET_KERNEL_INTERRUPT_TABLE,
        table,
        table_bytes,
        timeout_ns
    );
    if (rc == OK) {
        rc = find_vectors(*table, leaf_count, is_display_enabled, vectors);
    }

    heap::ufree(table);

    return rc;
}

int32_t read_nonstall_vector(firmware& fw, uint32_t engine_type, uint32_t leaf_count, uint32_t& vector) {
    uint16_t engine = 0;
    if (!find_engine_number(engine_type, engine)) {
        log::error(
            "nvidia: engine type 0x%X names neither the graphics engine nor a copy engine, so no nonstall vector is "
            "read for it",
            engine_type
        );
        return ERR_INVALID_ARG;
    }

    uint32_t table_bytes = sizeof(rm::kernel_interrupt_table_params);
    auto* table = static_cast<rm::kernel_interrupt_table_params*>(heap::uzalloc(table_bytes));
    if (table == nullptr) {
        log::error("nvidia: allocating %u bytes for the GSP firmware's kernel interrupt table failed", table_bytes);
        return ERR_NO_MEMORY;
    }

    int32_t rc = fw.internal_control(rm::CONTROL_GET_KERNEL_INTERRUPT_TABLE, table, table_bytes);
    if (rc == OK) {
        rc = check_row_count(*table);
    }

    if (rc == OK) {
        rc = find_nonstall_vector(*table, engine_type, engine, leaf_count, vector);
    }

    heap::ufree(table);

    return rc;
}

void enable_cpu_interrupt_tree(registers& regs, uint32_t leaf_count, const interrupt_vectors& vectors) {
    clear_leaf_enables(regs, leaf_count);

    uint32_t subtree_bits = subtree_bit(vectors.gsp);
    regs.write(leaf_register(CPU_INTERRUPT_LEAF_ENABLE_SET, leaf_of(vectors.gsp)), vector_bit(vectors.gsp));
    if (vectors.has_display) {
        uint32_t display_leaf = leaf_of(vectors.display);
        regs.write(leaf_register(CPU_INTERRUPT_LEAF_ENABLE_SET, display_leaf), vector_bit(vectors.display));
        subtree_bits |= subtree_bit(vectors.display);
    }

    regs.write(CPU_INTERRUPT_TOP_ENABLE_SET, subtree_bits);
}

void disable_cpu_interrupt_tree(registers& regs, uint32_t leaf_count) {
    regs.write(CPU_INTERRUPT_TOP_ENABLE_CLEAR, EVERY_BIT);
    clear_leaf_enables(regs, leaf_count);
}

void enable_vector(registers& regs, uint32_t vector) {
    regs.write(leaf_register(CPU_INTERRUPT_LEAF_ENABLE_SET, leaf_of(vector)), vector_bit(vector));
    regs.write(CPU_INTERRUPT_TOP_ENABLE_SET, subtree_bit(vector));
}

void mask_vector(registers& regs, uint32_t vector) {
    regs.write(leaf_register(CPU_INTERRUPT_LEAF_ENABLE_CLEAR, leaf_of(vector)), vector_bit(vector));
}

gsp_vector_reading take_gsp_interrupt(registers& regs, uint32_t vector) {
    uint32_t leaf = leaf_of(vector);
    uint32_t bit = vector_bit(vector);
    uint32_t pending = regs.read(leaf_register(CPU_INTERRUPT_LEAF, leaf));
    if (pending == UNRESPONSIVE_CARD_LEAF) {
        return gsp_vector_reading::all_ones;
    }

    if ((pending & bit) == 0) {
        return gsp_vector_reading::idle;
    }

    // NVIDIA's driver disables the GPU's interrupts until its bottom half ends, but the display's vertical blanks must
    // keep arriving while the task runs
    regs.write(leaf_register(CPU_INTERRUPT_LEAF_ENABLE_CLEAR, leaf), bit);
    regs.write(leaf_register(CPU_INTERRUPT_LEAF, leaf), bit);

    return gsp_vector_reading::taken;
}

void service_gsp_interrupt(registers& regs, gsp& conversation, uint32_t vector) {
    uint32_t status = regs.read(GSP_IRQ_STATUS);
    uint32_t falcon_sources = status & regs.read(GSP_IRQ_MASK) & regs.read(GSP_IRQ_DESTINATION);
    uint32_t riscv_sources = status & regs.read(GSP_RISCV_IRQ_MASK) & regs.read(GSP_RISCV_IRQ_DESTINATION);
    uint32_t sources = falcon_sources | riscv_sources;
    if (sources == UNRESPONSIVE_CARD_SOURCES) {
        log::error(
            "nvidia: the GSP falcon's interrupt registers read all ones, as on a card that stopped responding, so "
            "no message is read, the GSP's health is not checked and no register is written"
        );
        return;
    }

    int32_t rc = OK;
    if (sources != 0) {
        // NVIDIA's driver clears only SWGEN0 and HALT, but a source left pending would re-raise the interrupt at once
        regs.write(GSP_IRQ_CLEAR, sources);

        // A CPU sequencer the drain reads must run before any log line
        rc = conversation.drain();
        (void)conversation.check_health();
        if (!conversation.is_in_lockdown()) {
            regs.write(GSP_INTERRUPT_RETRIGGER, TRIGGER_BIT);
        }
    }

    regs.write(leaf_register(CPU_INTERRUPT_LEAF_ENABLE_SET, leaf_of(vector)), vector_bit(vector));

    uint32_t other_sources = sources & ~GSP_MESSAGE_SOURCE;
    if (other_sources != 0) {
        log::warn(
            "nvidia: the GSP falcon's interrupt registers read sources 0x%X pending for the host besides the "
            "firmware's message signal SWGEN0, cleared by the driver task",
            other_sources
        );
    }

    if (rc != OK) {
        log::error("nvidia: reading the GSP firmware's messages failed: %d", rc);
    }
}

bool acknowledge_display_interrupt(registers& regs, uint32_t vector) {
    uint32_t leaf = leaf_register(CPU_INTERRUPT_LEAF, leaf_of(vector));
    uint32_t bit = vector_bit(vector);
    if ((regs.read(leaf) & bit) == 0) {
        return false;
    }

    regs.write(leaf, bit);
    return true;
}

bool acknowledge_nonstall_interrupt(registers& regs, uint32_t vector) {
    uint32_t leaf = leaf_register(CPU_INTERRUPT_LEAF, leaf_of(vector));
    uint32_t bit = vector_bit(vector);
    if ((regs.read(leaf) & bit) == 0) {
        return false;
    }

    regs.write(leaf, bit);
    (void)regs.read(leaf);
    return true;
}

void rearm_msi(registers& regs) {
    regs.write(MSI_REARM, MSI_REARM_VALUE);
}

} // namespace drivers::nvidia
