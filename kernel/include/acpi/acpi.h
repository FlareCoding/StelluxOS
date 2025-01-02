#ifndef ACPI_H
#define ACPI_H
#include <types.h>

namespace acpi {
// ACPI RSDP (Root System Description Pointer)
struct rsdp_descriptor {
    char        signature[8];       // "RSD PTR "
    uint8_t     checksum;           // Checksum of first 20 bytes
    char        oem_id[6];          // OEM identifier
    uint8_t     revision;           // 0 for ACPI 1.0, 2 for ACPI 2.0+
    uint32_t    rsdt_address;       // Physical address of RSDT (32-bit)
    
    // Fields available in ACPI 2.0+
    uint32_t    length;             // Total size of the table, including extended fields
    uint64_t    xsdt_address;       // Physical address of XSDT (64-bit)
    uint8_t     extended_checksum;  // Checksum of entire table
    uint8_t     reserved[3];        // Reserved bytes
} __attribute__((packed));

struct acpi_sdt_header {
    char        signature[4];     // Table signature (e.g., "XSDT", "FACP", "APIC")
    uint32_t    length;           // Length of the table, including the header
    uint8_t     revision;         // Revision of the structure
    uint8_t     checksum;         // Checksum of the table
    char        oem_id[6];        // OEM identifier
    char        oem_table_id[8];  // OEM table identifier
    uint32_t    oem_revision;     // OEM revision
    uint32_t    creator_id;       // ID of the table creator
    uint32_t    creator_revision; // Revision of the table creator
} __attribute__((packed));

struct xsdt {
    acpi_sdt_header header; // Common SDT header
    uint64_t entries[];     // Array of table pointers (physical addresses)
} __attribute__((packed));

struct acpi_table {
    acpi_sdt_header* header;  // Pointer to the header
    void* data;               // Pointer to the table data
};

/**
 * @brief Enumerates and processes ACPI tables starting from the provided RSDP.
 * @param rsdp Pointer to the Root System Description Pointer (RSDP).
 * 
 * This function parses the ACPI tables starting from the given RSDP and performs any required initialization
 * or processing for each detected table. It is marked as privileged code.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void enumerate_acpi_tables(void* rsdp);
} // namespace acpi
#endif // ACPI_H
