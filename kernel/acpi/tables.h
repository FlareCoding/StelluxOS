#ifndef STELLUX_ACPI_TABLES_H
#define STELLUX_ACPI_TABLES_H

#include "common/types.h"

namespace acpi {

// RSDP signature: "RSD PTR " (8 bytes, trailing space)
constexpr char RSDP_SIGNATURE[8] = {'R','S','D',' ','P','T','R',' '};
constexpr size_t RSDP_V1_SIZE = 20;
constexpr size_t RSDP_V2_SIZE = 36;

struct __attribute__((packed)) rsdp {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision; // 0 = ACPI 1.0, 2 = ACPI 2.0+
    uint32_t rsdt_address;
    // ACPI 2.0+ fields (only valid when revision >= 2)
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
};

static_assert(sizeof(rsdp) == 36, "RSDP struct must be 36 bytes");

struct __attribute__((packed)) sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

static_assert(sizeof(sdt_header) == 36, "SDT header must be 36 bytes");

struct __attribute__((packed)) madt_table {
    sdt_header header; // signature = "APIC"
    uint32_t   local_apic_address;
    uint32_t   flags;
};

static_assert(sizeof(madt_table) == 44, "MADT table header must be 44 bytes");

constexpr uint32_t MADT_FLAG_PCAT_COMPAT = (1 << 0);

struct __attribute__((packed)) madt_entry_header {
    uint8_t type;
    uint8_t length;
};

static_assert(sizeof(madt_entry_header) == 2, "MADT entry header must be 2 bytes");

// MADT entry type constants
constexpr uint8_t MADT_TYPE_LOCAL_APIC           = 0x00;
constexpr uint8_t MADT_TYPE_IO_APIC              = 0x01;
constexpr uint8_t MADT_TYPE_INT_SRC_OVERRIDE     = 0x02;
constexpr uint8_t MADT_TYPE_LOCAL_APIC_NMI       = 0x04;
constexpr uint8_t MADT_TYPE_LAPIC_ADDR_OVERRIDE  = 0x05;
constexpr uint8_t MADT_TYPE_GICC                 = 0x0B;
constexpr uint8_t MADT_TYPE_GICD                 = 0x0C;
constexpr uint8_t MADT_TYPE_GIC_MSI_FRAME        = 0x0D;
constexpr uint8_t MADT_TYPE_GICR                 = 0x0E;

/**
 * Validate ACPI checksum: sum of all bytes must be 0 (mod 256).
 */
inline bool validate_checksum(const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += p[i];
    }
    return sum == 0;
}

} // namespace acpi

#endif // STELLUX_ACPI_TABLES_H
