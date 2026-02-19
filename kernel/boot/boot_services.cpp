#include "boot_services.h"
#include "common/limine.h"

// Global boot info instance
__PRIVILEGED_DATA boot_info g_boot_info = {};

// Limine base revision - required by protocol
__attribute__((used, section(".limine_requests")))
volatile LIMINE_BASE_REVISION(3);

// Kernel address request - provides virtual and physical base addresses
__attribute__((used, section(".limine_requests")))
static volatile limine_kernel_address_request kernel_address_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0,
    .response = nullptr
};

// HHDM request - provides higher half direct map offset
__attribute__((used, section(".limine_requests")))
static volatile limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0,
    .response = nullptr
};

// Memory map request - provides system memory regions
__attribute__((used, section(".limine_requests")))
static volatile limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
    .response = nullptr
};

// RSDP request - provides ACPI root pointer
__attribute__((used, section(".limine_requests")))
static volatile limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0,
    .response = nullptr
};

// DTB request - provides Device Tree Blob (aarch64)
__attribute__((used, section(".limine_requests")))
static volatile limine_dtb_request dtb_request = {
    .id = LIMINE_DTB_REQUEST,
    .revision = 0,
    .response = nullptr
};

// Kernel file request - provides kernel ELF info
__attribute__((used, section(".limine_requests")))
static volatile limine_kernel_file_request kernel_file_request = {
    .id = LIMINE_KERNEL_FILE_REQUEST,
    .revision = 0,
    .response = nullptr
};

// Module request - provides loaded modules
__attribute__((used, section(".limine_requests")))
static volatile limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0,
    .response = nullptr,
    .internal_module_count = 0,
    .internal_modules = nullptr
};

namespace boot_services {

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
    // Verify boot protocol was accepted
    if (limine_base_revision[2] != 0) {
        return ERR_NOT_AVAILABLE;
    }

    // Get kernel address info (required)
    if (!LIMINE_REQUEST_FULFILLED(kernel_address_request)) {
        return ERR_NOT_AVAILABLE;
    }
    g_boot_info.kernel_phys_base = kernel_address_request.response->physical_base;
    g_boot_info.kernel_virt_base = kernel_address_request.response->virtual_base;

    // Get HHDM offset (required)
    if (!LIMINE_REQUEST_FULFILLED(hhdm_request)) {
        return ERR_NOT_AVAILABLE;
    }
    g_boot_info.hhdm_offset = hhdm_request.response->offset;

    // Get memory map (required)
    if (!LIMINE_REQUEST_FULFILLED(memmap_request)) {
        return ERR_NOT_AVAILABLE;
    }
    g_boot_info.memmap_entry_count = memmap_request.response->entry_count;
    g_boot_info.memmap_entries = memmap_request.response->entries;

    // Compute highest physical address (any entry type — HHDM spans the full range)
    uint64_t max_phys = 0;
    for (uint64_t i = 0; i < g_boot_info.memmap_entry_count; i++) {
        uint64_t end = g_boot_info.memmap_entries[i]->base +
                       g_boot_info.memmap_entries[i]->length;
        if (end > max_phys) {
            max_phys = end;
        }
    }
    g_boot_info.max_phys_addr = (max_phys + 0xFFF) & ~static_cast<uint64_t>(0xFFF);

    // Get RSDP (optional - may not be available on all platforms)
    if (LIMINE_REQUEST_FULFILLED(rsdp_request)) {
        g_boot_info.rsdp_address = rsdp_request.response->address;
    } else {
        g_boot_info.rsdp_address = nullptr;
    }

    // Get DTB (optional - typically only on aarch64)
    if (LIMINE_REQUEST_FULFILLED(dtb_request)) {
        g_boot_info.dtb_ptr = dtb_request.response->dtb_ptr;
    } else {
        g_boot_info.dtb_ptr = nullptr;
    }

    // Get kernel file info (optional)
    if (LIMINE_REQUEST_FULFILLED(kernel_file_request)) {
        g_boot_info.kernel_file = kernel_file_request.response->kernel_file;
    } else {
        g_boot_info.kernel_file = nullptr;
    }

    // Get modules (optional)
    if (LIMINE_REQUEST_FULFILLED(module_request)) {
        g_boot_info.module_count = module_request.response->module_count;
        g_boot_info.modules = module_request.response->modules;
    } else {
        g_boot_info.module_count = 0;
        g_boot_info.modules = nullptr;
    }

    return OK;
}

} // namespace boot_services
