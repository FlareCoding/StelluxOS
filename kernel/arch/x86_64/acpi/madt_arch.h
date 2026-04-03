#ifndef STELLUX_X86_64_ACPI_MADT_ARCH_H
#define STELLUX_X86_64_ACPI_MADT_ARCH_H

#include "common/types.h"
#include "acpi/tables.h"

namespace acpi {

// x86 MADT entry structs (packed, matching ACPI spec byte layouts)

struct __attribute__((packed)) madt_local_apic {
    madt_entry_header header; // type=0, length=8
    uint8_t  acpi_processor_id;
    uint8_t  apic_id;
    uint32_t flags;
};

constexpr uint32_t LAPIC_FLAG_ENABLED        = (1 << 0);
constexpr uint32_t LAPIC_FLAG_ONLINE_CAPABLE = (1 << 1);

struct __attribute__((packed)) madt_io_apic {
    madt_entry_header header; // type=1, length=12
    uint8_t  id;
    uint8_t  reserved;
    uint32_t address;
    uint32_t global_irq_base;
};

struct __attribute__((packed)) madt_int_src_override {
    madt_entry_header header; // type=2, length=10
    uint8_t  bus;
    uint8_t  source;
    uint32_t global_irq;
    uint16_t flags;
};

struct __attribute__((packed)) madt_local_apic_nmi {
    madt_entry_header header; // type=4, length=6
    uint8_t  acpi_processor_id; // 0xFF = all processors
    uint16_t flags;
    uint8_t  lint; // LINT pin (0 or 1)
};

struct __attribute__((packed)) madt_lapic_addr_override {
    madt_entry_header header; // type=5, length=12
    uint16_t reserved;
    uint64_t address;
};

// Parsed MADT result

// Static capacity limits (not spec-defined; ACPI allows arbitrary counts, but these values are safe in practice)
constexpr size_t MAX_IO_APICS = 16;
constexpr size_t MAX_ISOS     = 48;
constexpr size_t MAX_NMIS     = 8;

struct lapic_entry {
    uint8_t apic_id;
    bool    enabled;
};

struct io_apic_entry {
    uint8_t  id;
    uint32_t address;
    uint32_t global_irq_base;
};

struct iso_entry {
    uint8_t  bus;
    uint8_t  source;
    uint32_t global_irq;
    uint16_t flags;
};

struct nmi_entry {
    uint8_t  processor_id;
    uint16_t flags;
    uint8_t  lint;
};

struct madt_info {
    uint64_t lapic_base;

    lapic_entry lapics[MAX_CPUS];
    uint32_t    lapic_count;

    io_apic_entry io_apics[MAX_IO_APICS];
    uint32_t      io_apic_count;

    iso_entry isos[MAX_ISOS];
    uint32_t  iso_count;

    nmi_entry nmis[MAX_NMIS];
    uint32_t  nmi_count;
};

/**
 * @brief Get the parsed MADT info (valid after acpi::madt::parse()).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE const madt_info& get_madt_info();

} // namespace acpi

#endif // STELLUX_X86_64_ACPI_MADT_ARCH_H
