#ifndef STELLUX_MM_EARLY_MMU_H
#define STELLUX_MM_EARLY_MMU_H

#include "types.h"

namespace early_mmu {

constexpr int32_t OK = 0;
constexpr int32_t ERR_NO_SLOTS = -1;
constexpr int32_t ERR_NOT_INITIALIZED = -2;

/**
 * Initialize early MMU subsystem.
 * On AArch64: Sets up static page tables for TTBR0.
 * On x86_64: No-op (port I/O doesn't require MMU setup).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * Map a physical device address for early boot access.
 * On AArch64: Creates identity mapping via TTBR0 page tables.
 * On x86_64: Returns phys_addr unchanged (port I/O or already mapped).
 * @return The virtual address to use, or 0 on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uintptr_t map_device(uintptr_t phys_addr, size_t size);

} // namespace early_mmu

#endif // STELLUX_MM_EARLY_MMU_H
