#ifndef STELLUX_MM_KVA_H
#define STELLUX_MM_KVA_H

#include "common/types.h"

namespace kva {

constexpr int32_t OK              = 0;
constexpr int32_t ERR_INVALID_ARG = -1;
constexpr int32_t ERR_ALIGNMENT   = -2;
constexpr int32_t ERR_NO_VIRT     = -3;
constexpr int32_t ERR_NOT_FOUND   = -4;
constexpr int32_t ERR_DOUBLE_FREE = -5;
constexpr int32_t ERR_NO_MEM      = -6;

enum class placement : uint8_t {
    low,  // allocate from lowest available (boot, heaps, device mmio)
    high, // allocate from highest available (stacks)
};

enum class tag : uint16_t {
    generic = 0,
    privileged_heap,
    unprivileged_heap,
    privileged_stack,
    unprivileged_stack,
    mmio,
    phys_map,
    boot,
};

// Describes a KVA allocation returned from alloc() and query().
struct allocation {
    uintptr_t base;          // usable start (after pre-guard pages)
    size_t    size;          // usable bytes (page-multiple)
    uintptr_t reserved_base; // full range start (including pre-guards)
    size_t    reserved_size; // full range size (including all guards)
    uint16_t  guard_pre;
    uint16_t  guard_post;
    tag       alloc_tag;
    uint8_t   pmm_order; // 0=non-contiguous/MMIO, 1-18=contiguous PMM order
};

/**
 * @brief Initialize the KVA allocator. Call after mm::init_va_layout().
 * @return OK on success, error code on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Reserve a contiguous VA range with optional guard pages.
 * @param size Usable bytes (must be > 0, rounded up to page boundary).
 * @param align Alignment (power-of-2, >= PAGE_SIZE).
 * @param guard_pre Guard pages before the usable region.
 * @param guard_post Guard pages after the usable region.
 * @param place Allocate from low or high end of VA space.
 * @param t Tag for debugging/accounting.
 * @param pmm_order 0 for non-contiguous, 1-18 for contiguous PMM order.
 * @param out Populated on success.
 * @return OK on success, error code on failure.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE int32_t alloc(
    size_t      size,
    size_t      align,
    uint16_t    guard_pre,
    uint16_t    guard_post,
    placement   place,
    tag         t,
    uint8_t     pmm_order,
    allocation& out
);

/**
 * @brief Free a previously allocated VA range by its usable base address.
 * Coalesces with adjacent free ranges.
 * @param base The usable base address returned by alloc().
 * @return OK on success, ERR_NOT_FOUND if not allocated.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t free(uintptr_t base);

/**
 * @brief Mark a fixed VA range as used (for pre-mapped regions).
 * The range must lie entirely within a single free region.
 * @return OK on success, ERR_NOT_FOUND if the range isn't free.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t reserve(uintptr_t base, size_t size, tag t);

/**
 * @brief Find which allocation contains addr.
 * Checks both usable and guard regions of used allocations.
 * @return OK if found, ERR_NOT_FOUND otherwise.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE int32_t query(uintptr_t addr, allocation& out);

/**
 * @brief Dump all free and used ranges to serial.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void dump_state();

} // namespace kva

#endif // STELLUX_MM_KVA_H
