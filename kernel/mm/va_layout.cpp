#include "mm/va_layout.h"
#include "boot/boot_services.h"
#include "common/logging.h"

extern "C" {
    extern char __stlx_kern_start[];
    extern char __stlx_kern_end[];
}

namespace mm {

__PRIVILEGED_DATA static va_layout g_layout = {};

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_va_layout() {
    if (g_boot_info.max_phys_addr == 0) {
        return VA_LAYOUT_ERR_NO_MAP;
    }

    g_layout.hhdm_base = g_boot_info.hhdm_offset;
    g_layout.hhdm_end = g_boot_info.hhdm_offset + g_boot_info.max_phys_addr;

    g_layout.kernel_image_base = reinterpret_cast<uintptr_t>(__stlx_kern_start);
    g_layout.kernel_image_end = reinterpret_cast<uintptr_t>(__stlx_kern_end);

    g_layout.kva_base = g_layout.hhdm_end;
    g_layout.kva_end = g_layout.kernel_image_base;

    log::info("VA layout:");
    log::info("  HHDM:   0x%016lx - 0x%016lx",
              g_layout.hhdm_base, g_layout.hhdm_end);
    log::info("  KVA:    0x%016lx - 0x%016lx (%lu GB)",
              g_layout.kva_base, g_layout.kva_end,
              (g_layout.kva_end - g_layout.kva_base) / (1024ULL * 1024 * 1024));
    log::info("  Kernel: 0x%016lx - 0x%016lx (%lu KB)",
              g_layout.kernel_image_base, g_layout.kernel_image_end,
              (g_layout.kernel_image_end - g_layout.kernel_image_base) / 1024);

    return VA_LAYOUT_OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE const va_layout& get_va_layout() {
    return g_layout;
}

} // namespace mm
