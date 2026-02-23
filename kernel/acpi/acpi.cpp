#include "acpi/acpi.h"
#include "acpi/tables.h"
#include "acpi/madt.h"
#include "boot/boot_services.h"
#include "mm/vmm.h"
#include "mm/paging_types.h"
#include "common/logging.h"
#include "common/string.h"

namespace acpi {

constexpr size_t XSDT_CACHE_CAPACITY = 64;

__PRIVILEGED_BSS static bool g_use_xsdt;
__PRIVILEGED_BSS static uintptr_t g_sdt_va;
__PRIVILEGED_BSS static size_t g_entry_count;
__PRIVILEGED_BSS static const sdt_header* g_table_cache[XSDT_CACHE_CAPACITY];

/**
 * Map a physical address range. Returns the usable virtual address.
 * Mapping is permanent (never freed).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static const void* map_acpi_region(
    uint64_t phys, size_t size
) {
    uintptr_t base = 0;
    uintptr_t va = 0;
    int32_t rc = vmm::map_phys(
        static_cast<pmm::phys_addr_t>(phys), size,
        paging::PAGE_KERNEL_RO, base, va);
    if (rc != vmm::OK) {
        return nullptr;
    }
    return reinterpret_cast<const void*>(va);
}

/**
 * Read a uint32_t from a potentially unaligned address.
 * Uses memcpy to avoid unaligned access faults on AArch64.
 */
__PRIVILEGED_CODE static uint32_t read_u32(const void* ptr) {
    uint32_t val;
    string::memcpy(&val, ptr, sizeof(val));
    return val;
}

/**
 * Read a uint64_t from a potentially unaligned address.
 */
__PRIVILEGED_CODE static uint64_t read_u64(const void* ptr) {
    uint64_t val;
    string::memcpy(&val, ptr, sizeof(val));
    return val;
}

/**
 * Get the physical address of the Nth XSDT/RSDT entry.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static uint64_t get_entry_phys(size_t index) {
    if (g_use_xsdt) {
        const auto* entries = reinterpret_cast<const uint8_t*>(g_sdt_va)
                            + sizeof(sdt_header);
        return read_u64(entries + index * 8);
    } else {
        const auto* entries = reinterpret_cast<const uint8_t*>(g_sdt_va)
                            + sizeof(sdt_header);
        return read_u32(entries + index * 4);
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
    if (g_boot_info.rsdp_address == nullptr) {
        log::error("acpi: no RSDP provided by bootloader");
        return ERR_NO_RSDP;
    }

    auto rsdp_phys = reinterpret_cast<uintptr_t>(g_boot_info.rsdp_address);

    // Map the RSDP
    auto* rsdp_ptr = static_cast<const rsdp*>(
        map_acpi_region(rsdp_phys, sizeof(rsdp)));
    if (!rsdp_ptr) {
        log::error("acpi: failed to map RSDP at 0x%lx", rsdp_phys);
        return ERR_MAP_FAILED;
    }

    // Validate RSDP signature
    if (string::memcmp(rsdp_ptr->signature, RSDP_SIGNATURE, 8) != 0) {
        log::error("acpi: invalid RSDP signature");
        return ERR_BAD_CHECKSUM;
    }

    // Validate v1 checksum (first 20 bytes)
    if (!validate_checksum(rsdp_ptr, RSDP_V1_SIZE)) {
        log::error("acpi: RSDP v1 checksum failed");
        return ERR_BAD_CHECKSUM;
    }

    uint64_t sdt_phys = 0;
    size_t entry_size = 0;

    if (rsdp_ptr->revision >= 2) {
        // ACPI 2.0+ -- validate extended checksum and use XSDT
        if (!validate_checksum(rsdp_ptr, RSDP_V2_SIZE)) {
            log::error("acpi: RSDP v2 extended checksum failed");
            return ERR_BAD_CHECKSUM;
        }
        sdt_phys = read_u64(&rsdp_ptr->xsdt_address);
        g_use_xsdt = true;
        entry_size = 8;
    } else {
        // ACPI 1.0 -- use RSDT
        sdt_phys = read_u32(&rsdp_ptr->rsdt_address);
        g_use_xsdt = false;
        entry_size = 4;
    }

    if (sdt_phys == 0) {
        log::error("acpi: XSDT/RSDT address is null");
        return ERR_NOT_FOUND;
    }

    // Map 1 page at the SDT to read the header
    auto* sdt_hdr = static_cast<const sdt_header*>(
        map_acpi_region(sdt_phys, pmm::PAGE_SIZE));
    if (!sdt_hdr) {
        log::error("acpi: failed to map XSDT/RSDT at 0x%lx", sdt_phys);
        return ERR_MAP_FAILED;
    }

    uint32_t sdt_length = read_u32(&sdt_hdr->length);

    // If the table exceeds 1 page, create a full-size mapping
    if (sdt_length > pmm::PAGE_SIZE) {
        sdt_hdr = static_cast<const sdt_header*>(
            map_acpi_region(sdt_phys, sdt_length));
        if (!sdt_hdr) {
            log::error("acpi: failed to map full XSDT/RSDT (%u bytes)", sdt_length);
            return ERR_MAP_FAILED;
        }
    }

    // Validate SDT checksum
    if (!validate_checksum(sdt_hdr, sdt_length)) {
        log::error("acpi: XSDT/RSDT checksum failed");
        return ERR_BAD_CHECKSUM;
    }

    g_sdt_va = reinterpret_cast<uintptr_t>(sdt_hdr);
    g_entry_count = (sdt_length - sizeof(sdt_header)) / entry_size;
    if (g_entry_count > XSDT_CACHE_CAPACITY) {
        g_entry_count = XSDT_CACHE_CAPACITY;
    }

    // Zero the cache
    for (size_t i = 0; i < XSDT_CACHE_CAPACITY; i++) {
        g_table_cache[i] = nullptr;
    }

    log::info("acpi: RSDP v%u at 0x%lx OEM=%.6s",
              static_cast<uint32_t>(rsdp_ptr->revision),
              rsdp_phys,
              rsdp_ptr->oem_id);
    log::info("acpi: %s at 0x%lx (%u tables)",
              g_use_xsdt ? "XSDT" : "RSDT",
              sdt_phys,
              static_cast<uint32_t>(g_entry_count));

    // Parse platform-specific tables
    int32_t madt_rc = madt::parse();
    if (madt_rc != OK) {
        log::error("acpi: MADT parse failed (%d)", madt_rc);
        return madt_rc;
    }
    madt::dump();

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE const sdt_header* find_table(const char signature[4]) {
    for (size_t i = 0; i < g_entry_count; i++) {
        const sdt_header* hdr = g_table_cache[i];

        if (!hdr) {
            uint64_t phys = get_entry_phys(i);
            if (phys == 0) continue;

            // Map 1 page to read the header
            hdr = static_cast<const sdt_header*>(
                map_acpi_region(phys, pmm::PAGE_SIZE));
            if (!hdr) continue;

            uint32_t length = read_u32(&hdr->length);

            // If table exceeds 1 page, create a full mapping
            if (length > pmm::PAGE_SIZE) {
                auto* full = static_cast<const sdt_header*>(
                    map_acpi_region(phys, length));
                if (full) {
                    hdr = full;
                }
            }

            g_table_cache[i] = hdr;
        }

        if (string::memcmp(hdr->signature, signature, 4) == 0) {
            uint32_t length = read_u32(&hdr->length);
            if (!validate_checksum(hdr, length)) {
                log::error("acpi: table '%.4s' checksum failed", signature);
                continue;
            }
            return hdr;
        }
    }

    return nullptr;
}

} // namespace acpi
