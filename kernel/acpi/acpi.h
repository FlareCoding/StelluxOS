#ifndef STELLUX_ACPI_ACPI_H
#define STELLUX_ACPI_ACPI_H

#include "common/types.h"
#include "acpi/tables.h"

namespace acpi {

constexpr int32_t OK               = 0;
constexpr int32_t ERR_NO_RSDP      = -1;
constexpr int32_t ERR_BAD_CHECKSUM = -2;
constexpr int32_t ERR_NOT_FOUND    = -3;
constexpr int32_t ERR_MAP_FAILED   = -4;

/**
 * @brief Initialize the ACPI subsystem. Validates RSDP, locates XSDT/RSDT.
 * Must be called after mm::init().
 * @return OK on success, negative error code on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Find an ACPI table by its 4-byte signature.
 * Maps the table on demand via vmm::map_phys() and caches the result.
 * Returned pointer is permanently valid (never unmapped).
 * @param signature 4-byte table signature (e.g., "APIC" for MADT).
 * @return Pointer to the table header, or nullptr if not found.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE const sdt_header* find_table(const char signature[4]);

} // namespace acpi

#endif // STELLUX_ACPI_ACPI_H
