#ifndef STELLUX_MM_VMM_H
#define STELLUX_MM_VMM_H

#include "common/types.h"
#include "mm/kva.h"
#include "mm/paging_types.h"
#include "mm/pmm_types.h"

namespace vmm {

constexpr int32_t OK              = 0;
constexpr int32_t ERR_INVALID_ARG = -1;
constexpr int32_t ERR_NO_VIRT     = -2;
constexpr int32_t ERR_NO_PHYS     = -3;
constexpr int32_t ERR_NO_MEM      = -4;
constexpr int32_t ERR_PAGING      = -5;
constexpr int32_t ERR_NOT_FOUND   = -6;

constexpr uint32_t ALLOC_ZERO      = (1 << 0);
constexpr uint32_t ALLOC_ALLOW_2MB = (1 << 1); // alloc_contiguous only
constexpr uint32_t ALLOC_ALLOW_1GB = (1 << 2); // alloc_contiguous only

/**
 * @brief Initialize the VMM. Call after kva::init().
 * @return OK on success.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Allocate non-contiguous pages (always 4KB).
 * Zeroing is forced if PAGE_USER is set in flags; optional via ALLOC_ZERO.
 * @param pages Number of 4KB pages (> 0).
 * @param flags Page permissions (PAGE_USER determines access level and zeroing).
 * @param alloc_flags ALLOC_ZERO only (large page flags ignored).
 * @param tag KVA allocation tag.
 * @param out Usable virtual address on success.
 * @return OK on success, error code on failure.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE int32_t alloc(
    size_t               pages,
    paging::page_flags_t flags,
    uint32_t             alloc_flags,
    kva::tag             tag,
    uintptr_t&           out
);

/**
 * @brief Allocate physically contiguous pages (single PMM block).
 * Page count is rounded up to 2^order. Large pages used when aligned.
 * @param pages Requested pages (rounded up to power of 2).
 * @param zone PMM zone mask (ZONE_DMA32, ZONE_NORMAL, ZONE_ANY).
 * @param flags Page permissions.
 * @param alloc_flags ALLOC_ZERO, ALLOC_ALLOW_2MB, ALLOC_ALLOW_1GB.
 * @param tag KVA allocation tag.
 * @param out_addr Usable virtual address on success.
 * @param out_phys Physical base address on success (for DMA).
 * @return OK on success, error code on failure.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE int32_t alloc_contiguous(
    size_t               pages,
    pmm::zone_mask_t     zone,
    paging::page_flags_t flags,
    uint32_t             alloc_flags,
    kva::tag             tag,
    uintptr_t&           out_addr,
    pmm::phys_addr_t&    out_phys
);

/**
 * @brief Allocate a stack with guard pages. Always zeroed.
 * Page flags are derived from tag (privileged_stack or unprivileged_stack).
 * Uses placement::high and guard_pre for downward-growing stacks.
 * @param usable_pages Number of usable stack pages (> 0).
 * @param guard_pages Number of guard pages below the usable region.
 * @param tag Must be privileged_stack or unprivileged_stack.
 * @param out_base Lowest usable VA on success.
 * @param out_top One past highest usable VA on success.
 * @return OK on success, error code on failure.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE int32_t alloc_stack(
    size_t     usable_pages,
    uint16_t   guard_pages,
    kva::tag   tag,
    uintptr_t& out_base,
    uintptr_t& out_top
);

/**
 * @brief Map a physical device region for MMIO (no physical allocation).
 * Physical address may be non-page-aligned; offset is handled internally.
 * Forces PAGE_DEVICE if no cache type (PAGE_DEVICE, PAGE_WC) is specified.
 * @param phys Physical address of device region.
 * @param size Size in bytes (> 0).
 * @param flags Page permissions. PAGE_DEVICE forced if no cache type set.
 * @param out_base Page-aligned KVA base (pass to free()).
 * @param out_va Virtual address for MMIO access (includes offset).
 * @return OK on success, error code on failure.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE int32_t map_device(
    pmm::phys_addr_t     phys,
    size_t               size,
    paging::page_flags_t flags,
    uintptr_t&           out_base,
    uintptr_t&           out_va
);

/**
 * @brief Map caller-provided physical pages into kernel VA (no physical allocation).
 * Unlike map_device, does not force any cache type — caller controls caching.
 * Useful for firmware tables, shared memory, framebuffers, etc.
 * Physical pages are not freed on vmm::free(); caller retains ownership.
 * @param phys Physical address (may be non-page-aligned).
 * @param size Size in bytes (> 0).
 * @param flags Page permissions and cache type (caller's choice).
 * @param out_base Page-aligned KVA base (pass to free()).
 * @param out_va Virtual address for access (includes offset).
 * @return OK on success, error code on failure.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE int32_t map_phys(
    pmm::phys_addr_t     phys,
    size_t               size,
    paging::page_flags_t flags,
    uintptr_t&           out_base,
    uintptr_t&           out_va
);

/**
 * @brief Free any VMM allocation by address.
 * Uses kva::query() to find allocation type, then unmaps and frees accordingly.
 * MMIO mappings are unmapped but physical pages are not freed.
 * @param addr Any address within the allocation (usable or guard region).
 * @return OK on success, ERR_NOT_FOUND if not allocated.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t free(uintptr_t addr);

/**
 * @brief Dump VMM state to serial (delegates to kva::dump_state).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void dump_state();

} // namespace vmm

#endif // STELLUX_MM_VMM_H
