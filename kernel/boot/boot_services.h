#ifndef STELLUX_BOOT_SERVICES_H
#define STELLUX_BOOT_SERVICES_H

#include "common/types.h"
#include "limine.h"

struct boot_info {
    // Kernel location
    uint64_t kernel_phys_base;
    uint64_t kernel_virt_base;
    uint64_t hhdm_offset;

    // Highest physical address (page-aligned, from memory map)
    uint64_t max_phys_addr;

    // Memory map (raw Limine pointers — only valid before paging::init()
    // replaces page tables, since entries live in bootloader-reclaimable memory)
    uint64_t memmap_entry_count;
    struct limine_memmap_entry** memmap_entries;

    // ACPI RSDP (physical address, nullptr if not available)
    void* rsdp_address;

    // Device Tree Blob (virtual address, nullptr if not available)
    void* dtb_ptr;

    // Kernel file info (nullptr if not available)
    struct limine_file* kernel_file;

    // Modules (nullptr/0 if none)
    uint64_t module_count;
    struct limine_file** modules;
};

__PRIVILEGED_DATA extern boot_info g_boot_info;

namespace boot_services {

constexpr int32_t OK = 0;
constexpr int32_t ERR_NOT_AVAILABLE = -1;

/**
 * @brief Initialize boot services and populate g_boot_info.
 * @return OK on success, error code on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

} // namespace boot_services

#endif // STELLUX_BOOT_SERVICES_H
