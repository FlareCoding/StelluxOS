#ifndef STELLUX_MM_VA_LAYOUT_H
#define STELLUX_MM_VA_LAYOUT_H

#include "common/types.h"

namespace mm {

constexpr int32_t VA_LAYOUT_OK          = 0;
constexpr int32_t VA_LAYOUT_ERR_NO_MAP  = -1;

// Describes the kernel's virtual address space regions.
// Computed once from boot info and linker symbols, then constant.
struct va_layout {
    uintptr_t hhdm_base;          // start of direct map
    uintptr_t hhdm_end;          // exclusive

    uintptr_t kernel_image_base;  // __stlx_kern_start
    uintptr_t kernel_image_end;  // __stlx_kern_end (page-aligned)

    uintptr_t kva_base;          // start of KVA-managed range (after HHDM)
    uintptr_t kva_end;           // exclusive (before kernel image)
};

/**
 * @brief Compute and store the VA layout from boot info and linker symbols.
 * Must be called after boot_services::init() and pmm::init().
 * @return VA_LAYOUT_OK on success.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_va_layout();

/**
 * @brief Return the computed VA layout. Must only be called after init_va_layout().
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE const va_layout& get_va_layout();

} // namespace mm

#endif // STELLUX_MM_VA_LAYOUT_H
