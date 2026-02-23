#ifndef STELLUX_ACPI_MADT_H
#define STELLUX_ACPI_MADT_H

#include "common/types.h"
#include "acpi/acpi.h"

namespace acpi::madt {

/**
 * @brief Parse the MADT table. Calls acpi::find_table("APIC") and
 * extracts arch-specific interrupt controller info into a static result struct.
 * @return acpi::OK on success, negative error code on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t parse();

/**
 * @brief Dump parsed MADT contents to the serial log.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void dump();

} // namespace acpi::madt

#endif // STELLUX_ACPI_MADT_H
