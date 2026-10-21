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

// Finds the GSP's vector in the table the reply filled
static int32_t find_gsp_vector(const rm::kernel_interrupt_table_params& table, uint32_t leaf_count, uint32_t& vector) {
    if (table.row_count > rm::KERNEL_INTERRUPT_ROWS_MAX) {
        log::error(
            "nvidia: the GSP firmware's kernel interrupt table counts %u rows, more than the %u it has room for",
            table.row_count,
            rm::KERNEL_INTERRUPT_ROWS_MAX
        );
        return ERR_BAD_MESSAGE;
    }

    uint32_t vector_count = leaf_count * VECTORS_PER_LEAF;
    for (uint32_t i = 0; i < table.row_count; i++) {
        const rm::kernel_interrupt_row& row = table.rows[i];
        if (row.engine != rm::ENGINE_GSP) {
            continue;
        }

        if (row.stall_vector >= vector_count) {
            log::error(
                "nvidia: the GSP firmware's kernel interrupt table gives the GSP vector %u, outside the %u vectors "
                "of the CPU interrupt tree",
                row.stall_vector,
                vector_count
            );
            return ERR_BAD_MESSAGE;
        }

        if (row.pmc_interrupt_mask != 0) {
            log::error(
                "nvidia: the GSP firmware's kernel interrupt table gives the GSP's row the PMC mask 0x%X, and "
                "NVIDIA's driver never enables the vector of a row with one",
                row.pmc_interrupt_mask
            );
            return ERR_BAD_MESSAGE;
        }

        vector = row.stall_vector;
        return OK;
    }

    log::error("nvidia: the GSP firmware's kernel interrupt table counts %u rows, none for the GSP", table.row_count);
    return ERR_BAD_MESSAGE;
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

int32_t read_gsp_interrupt_vector(
    gsp& conversation,
    rm::handle client,
    rm::handle subdevice,
    uint32_t leaf_count,
    uint64_t timeout_ns,
    uint32_t& vector
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
        rc = find_gsp_vector(*table, leaf_count, vector);
    }

    heap::ufree(table);

    return rc;
}

void enable_cpu_interrupt_tree(registers& regs, uint32_t leaf_count, uint32_t gsp_vector) {
    clear_leaf_enables(regs, leaf_count);

    regs.write(leaf_register(CPU_INTERRUPT_LEAF_ENABLE_SET, leaf_of(gsp_vector)), vector_bit(gsp_vector));
    regs.write(CPU_INTERRUPT_TOP_ENABLE_SET, subtree_bit(gsp_vector));
}

void disable_cpu_interrupt_tree(registers& regs, uint32_t leaf_count) {
    regs.write(CPU_INTERRUPT_TOP_ENABLE_CLEAR, EVERY_BIT);
    clear_leaf_enables(regs, leaf_count);
}

uint32_t acknowledge_gsp_interrupt(registers& regs, uint32_t vector) {
    regs.write(leaf_register(CPU_INTERRUPT_LEAF, leaf_of(vector)), vector_bit(vector));

    uint32_t status = regs.read(GSP_IRQ_STATUS);
    uint32_t falcon_sources = status & regs.read(GSP_IRQ_MASK) & regs.read(GSP_IRQ_DESTINATION);
    uint32_t riscv_sources = status & regs.read(GSP_RISCV_IRQ_MASK) & regs.read(GSP_RISCV_IRQ_DESTINATION);
    uint32_t sources = falcon_sources | riscv_sources;
    if (sources == 0) {
        return 0;
    }

    regs.write(GSP_IRQ_CLEAR, sources);
    regs.write(GSP_INTERRUPT_RETRIGGER, TRIGGER_BIT);

    return sources;
}

void rearm_msi(registers& regs) {
    regs.write(MSI_REARM, MSI_REARM_VALUE);
}

} // namespace drivers::nvidia
