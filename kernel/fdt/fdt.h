#ifndef STELLUX_FDT_FDT_H
#define STELLUX_FDT_FDT_H

#include "common/types.h"

namespace fdt {

constexpr int32_t OK            = 0;
constexpr int32_t ERR_NO_DTB    = -1;
constexpr int32_t ERR_BAD_MAGIC = -2;
constexpr int32_t ERR_NOT_FOUND = -3;
constexpr int32_t ERR_BAD_DATA  = -4;

constexpr uint32_t FDT_MAGIC      = 0xd00dfeed;
constexpr uint32_t FDT_BEGIN_NODE = 0x00000001;
constexpr uint32_t FDT_END_NODE   = 0x00000002;
constexpr uint32_t FDT_PROP       = 0x00000003;
constexpr uint32_t FDT_NOP        = 0x00000004;
constexpr uint32_t FDT_END        = 0x00000009;

struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
} __attribute__((packed));
static_assert(sizeof(fdt_header) == 40);

/**
 * Initialize the FDT parser. Maps the DTB into kernel VA using
 * dtb_phys/dtb_size saved by boot_services. Call after mm::init().
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * Find a node whose "compatible" property contains the given string.
 * Returns offset into the structure block, or ERR_NOT_FOUND.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t find_compatible(const char* compatible);

/**
 * Read a raw property from the node at the given structure offset.
 * Returns pointer to property data and sets out_len, or nullptr.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE const void* get_prop(int32_t node_offset, const char* name,
                                       uint32_t* out_len);

/**
 * Read first "reg" entry as CPU physical base + size.
 * Handles parent #address-cells/#size-cells and single-level ranges translation.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t get_reg(int32_t node_offset,
                                  uint64_t* out_base, uint64_t* out_size);

/**
 * Read GIC interrupt numbers from a node's "interrupts" property.
 * Returns the number of IRQs found, or a negative error code.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t get_interrupts(int32_t node_offset,
                                         uint32_t* out_irqs, uint32_t max_irqs);

} // namespace fdt

#endif // STELLUX_FDT_FDT_H
