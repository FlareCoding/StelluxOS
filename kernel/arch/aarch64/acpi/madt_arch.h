#ifndef STELLUX_AARCH64_ACPI_MADT_ARCH_H
#define STELLUX_AARCH64_ACPI_MADT_ARCH_H

#include "common/types.h"
#include "acpi/tables.h"

namespace acpi {

// AArch64 MADT entry structs (packed, matching ACPI spec byte layouts)
// GICC is variable-length across ACPI versions; we define the full struct
// but only access fields within the entry's declared length.

struct __attribute__((packed)) madt_gicc {
    madt_entry_header header; // type=0x0B
    uint16_t reserved;
    uint32_t cpu_interface_number;
    uint32_t acpi_processor_uid;
    uint32_t flags;
    uint32_t parking_protocol_version;
    uint32_t performance_interrupt;
    uint64_t parked_address;
    uint64_t base_address;
    uint64_t gicv_base_address;
    uint64_t gich_base_address;
    uint32_t vgic_maintenance_interrupt;
    uint64_t gicr_base_address;
    uint64_t arm_mpidr;
    uint8_t  efficiency_class; // ACPI 6.0+
    uint8_t  reserved2;
    uint16_t spe_interrupt;    // ACPI 6.3+
};

constexpr size_t GICC_MIN_LENGTH = 76; // ACPI 5.1 minimum (up to arm_mpidr)
constexpr uint32_t GICC_FLAG_ENABLED        = (1 << 0);
constexpr uint32_t GICC_FLAG_ONLINE_CAPABLE = (1 << 3);

struct __attribute__((packed)) madt_gicd {
    madt_entry_header header; // type=0x0C, length=24
    uint16_t reserved;
    uint32_t gic_id;
    uint64_t base_address;
    uint32_t global_irq_base;
    uint8_t  version; // 0=none, 1=v1, 2=v2, 3=v3, 4=v4
    uint8_t  reserved2[3];
};

static_assert(sizeof(madt_gicd) == 24, "GICD entry must be 24 bytes");

struct __attribute__((packed)) madt_gic_msi_frame {
    madt_entry_header header; // type=0x0D, length=24
    uint16_t reserved;
    uint32_t gic_msi_frame_id;
    uint64_t base_address;
    uint32_t flags;
    uint16_t spi_count;
    uint16_t spi_base;
};

static_assert(sizeof(madt_gic_msi_frame) == 24, "GIC MSI Frame entry must be 24 bytes");

constexpr uint32_t GIC_MSI_FRAME_FLAG_SPI_SELECT = (1 << 0);

struct __attribute__((packed)) madt_gicr {
    madt_entry_header header; // type=0x0E, length=16
    uint8_t  flags;
    uint8_t  reserved;
    uint64_t base_address;
    uint32_t length;
};

static_assert(sizeof(madt_gicr) == 16, "GICR entry must be 16 bytes");

// Parsed MADT result

struct gicc_entry {
    uint64_t base_address;
    uint64_t mpidr;
    bool     enabled;
};

struct gic_msi_frame_entry {
    uint64_t base_address;
    uint32_t flags;
    uint16_t spi_count;
    uint16_t spi_base;
};

struct madt_info {
    uint64_t gicd_base;
    uint8_t  gic_version;

    gicc_entry giccs[MAX_CPUS];
    uint32_t   cpu_count;

    uint64_t gicr_base;
    uint32_t gicr_length;

    gic_msi_frame_entry msi_frame;
};

/**
 * @brief Get the parsed MADT info (valid after acpi::madt::parse()).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE const madt_info& get_madt_info();

} // namespace acpi

#endif // STELLUX_AARCH64_ACPI_MADT_ARCH_H
