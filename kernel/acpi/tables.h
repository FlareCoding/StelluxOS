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

// Generic address structure address space IDs
constexpr uint8_t GAS_SPACE_SYSTEM_MEMORY = 0;
constexpr uint8_t GAS_SPACE_SYSTEM_IO     = 1;
constexpr uint8_t GAS_SPACE_PCI_CONFIG    = 2;

struct __attribute__((packed)) generic_address {
    uint8_t  space;       // Address space ID (GAS_SPACE_*)
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
};

static_assert(sizeof(generic_address) == 12, "GAS must be 12 bytes");

// The ACPI 1.0 fixed block ends after the flags field. Later revisions
// appended fields, so anything past this offset needs a length check.
constexpr uint32_t FADT_REV1_LENGTH = 116;

// FADT fixed feature flags
constexpr uint32_t FADT_FLAG_RESET_REG_SUP = (1 << 10);

// FADT arm_boot_arch flags (ACPI 5.1+)
constexpr uint16_t FADT_ARM_PSCI_COMPLIANT = (1 << 0);
constexpr uint16_t FADT_ARM_PSCI_USE_HVC   = (1 << 1);

struct __attribute__((packed)) fadt {
    sdt_header header;               // signature = "FACP"
    uint32_t   firmware_ctrl;
    uint32_t   dsdt;
    uint8_t    reserved0;
    uint8_t    preferred_pm_profile;
    uint16_t   sci_int;
    uint32_t   smi_cmd;
    uint8_t    acpi_enable;
    uint8_t    acpi_disable;
    uint8_t    s4bios_req;
    uint8_t    pstate_cnt;
    uint32_t   pm1a_evt_blk;
    uint32_t   pm1b_evt_blk;
    uint32_t   pm1a_cnt_blk;
    uint32_t   pm1b_cnt_blk;
    uint32_t   pm2_cnt_blk;
    uint32_t   pm_tmr_blk;
    uint32_t   gpe0_blk;
    uint32_t   gpe1_blk;
    uint8_t    pm1_evt_len;
    uint8_t    pm1_cnt_len;
    uint8_t    pm2_cnt_len;
    uint8_t    pm_tmr_len;
    uint8_t    gpe0_blk_len;
    uint8_t    gpe1_blk_len;
    uint8_t    gpe1_base;
    uint8_t    cst_cnt;
    uint16_t   p_lvl2_lat;
    uint16_t   p_lvl3_lat;
    uint16_t   flush_size;
    uint16_t   flush_stride;
    uint8_t    duty_offset;
    uint8_t    duty_width;
    uint8_t    day_alrm;
    uint8_t    mon_alrm;
    uint8_t    century;              // CMOS century register index, 0 if absent
    uint16_t   iapc_boot_arch;
    uint8_t    reserved1;
    uint32_t   flags;
    generic_address reset_reg;
    uint8_t    reset_value;
    uint16_t   arm_boot_arch;        // FADT_ARM_* flags (ACPI 5.1+)
    uint8_t    fadt_minor_version;
    uint64_t   x_firmware_ctrl;
    uint64_t   x_dsdt;
    generic_address x_pm1a_evt_blk;
    generic_address x_pm1b_evt_blk;
    generic_address x_pm1a_cnt_blk;
    generic_address x_pm1b_cnt_blk;
    generic_address x_pm2_cnt_blk;
    generic_address x_pm_tmr_blk;
    generic_address x_gpe0_blk;
    generic_address x_gpe1_blk;
    generic_address sleep_control_reg;
    generic_address sleep_status_reg;
    uint64_t   hypervisor_id;
};

static_assert(sizeof(fadt) == 276, "FADT must be 276 bytes");
static_assert(__builtin_offsetof(fadt, century) == 108);
static_assert(__builtin_offsetof(fadt, flags) == 112);
static_assert(__builtin_offsetof(fadt, reset_reg) == FADT_REV1_LENGTH);
static_assert(__builtin_offsetof(fadt, arm_boot_arch) == 129);
static_assert(__builtin_offsetof(fadt, x_dsdt) == 140);

// Check that a FADT is long enough to contain a given field, since
// firmware ships whatever revision of the table it was built against.
#define FADT_HAS_FIELD(f, field) \
    ((f)->header.length >= \
     __builtin_offsetof(acpi::fadt, field) + sizeof((f)->field))

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
