// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/interrupts.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "common/logging.h"
#include "mm/heap.h"

namespace drivers::nvidia {

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

        vector = row.stall_vector;
        return OK;
    }

    log::error("nvidia: the GSP firmware's kernel interrupt table counts %u rows, none for the GSP", table.row_count);
    return ERR_BAD_MESSAGE;
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

} // namespace drivers::nvidia
