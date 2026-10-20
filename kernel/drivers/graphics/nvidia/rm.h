// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_RM_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_RM_H

#include "common/types.h"

// The vocabulary of the resource manager inside the GSP's firmware (the part of NVIDIA's firmware that keeps the GPU's
// objects and carries out the commands to them, called controls), only what this driver uses
namespace drivers::nvidia::rm {

// NVIDIA's handle, the number naming an object the firmware keeps, such as a client (the owner of a tree of objects)
using handle = uint32_t;

// NVIDIA's NV2080_CTRL_CMD_INTERNAL_INTR_GET_KERNEL_TABLE, the control that gives the kernel interrupt table (for each
// GPU unit whose interrupts NVIDIA's driver services, the vectors on which they arrive)
constexpr uint32_t CONTROL_GET_KERNEL_INTERRUPT_TABLE = 0x20800A5C;

// The most rows the kernel interrupt table holds, NVIDIA's NV2080_CTRL_INTERNAL_INTR_MAX_TABLE_SIZE
constexpr uint32_t KERNEL_INTERRUPT_ROWS_MAX = 128;

// NVIDIA's MC_ENGINE_IDX_GSP, the GSP's number among the engines (the GPU's units that NVIDIA's interrupt code serves),
// which the GSP's row in the kernel interrupt table gives
constexpr uint16_t ENGINE_GSP = 49;

// A row of the kernel interrupt table: an engine, a mask this driver does not read (NVIDIA's pmcIntrMask) and two GPU
// interrupt vectors (NVIDIA's numbers for the GPU's interrupts)
struct kernel_interrupt_row {
    uint16_t engine;
    uint32_t pmc_interrupt_mask;
    uint32_t stall_vector;    // for the interrupts NVIDIA's driver services with the engine's handler
    uint32_t nonstall_vector; // for the engine's notifications
};

// NVIDIA's NV2080_INTR_CATEGORY_SUBTREE_MAP, the first and last of the top-level bits a category of interrupts takes in
// the CPU interrupt tree (the GPU's registers reporting its interrupts to the CPU)
struct kernel_interrupt_subtree_range {
    uint8_t first;
    uint8_t last;
};

// NVIDIA's NV2080_INTR_CATEGORY_ENUM_COUNT, the categories of interrupts in the subtree map (for each category, a range
// of those top-level bits)
constexpr uint32_t KERNEL_INTERRUPT_CATEGORY_COUNT = 7;

// The parameters of `CONTROL_GET_KERNEL_INTERRUPT_TABLE`, which the reply fills. The subtree map is not read
struct kernel_interrupt_table_params {
    uint32_t                       row_count;
    kernel_interrupt_row           rows[KERNEL_INTERRUPT_ROWS_MAX];
    kernel_interrupt_subtree_range subtree_map[KERNEL_INTERRUPT_CATEGORY_COUNT];
};

static_assert(sizeof(kernel_interrupt_row) == 16, "NVIDIA's layout of a row of the kernel interrupt table");
static_assert(sizeof(kernel_interrupt_subtree_range) == 2, "NVIDIA's layout of a range of the subtree map");
static_assert(sizeof(kernel_interrupt_table_params) == 2068, "NVIDIA's layout of the kernel interrupt table");

} // namespace drivers::nvidia::rm

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_RM_H
