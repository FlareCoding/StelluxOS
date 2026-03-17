#include "acpi/madt.h"
#include "acpi/acpi.h"
#include "acpi/tables.h"
#include "acpi/madt_arch.h"
#include "common/logging.h"
#include "common/string.h"

namespace acpi {

__PRIVILEGED_DATA static madt_info g_madt = {};

__PRIVILEGED_CODE static uint32_t read_u32_safe(const void* ptr) {
    uint32_t val;
    string::memcpy(&val, ptr, sizeof(val));
    return val;
}

__PRIVILEGED_CODE static uint64_t read_u64_safe(const void* ptr) {
    uint64_t val;
    string::memcpy(&val, ptr, sizeof(val));
    return val;
}

__PRIVILEGED_CODE const madt_info& get_madt_info() {
    return g_madt;
}

namespace madt {

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t parse() {
    const auto* table = find_table("APIC");
    if (!table) {
        log::error("madt: MADT table not found");
        return ERR_NOT_FOUND;
    }

    const auto* m = reinterpret_cast<const acpi::madt_table*>(table);
    g_madt.gicd_base = 0;
    g_madt.gic_version = 0;
    g_madt.cpu_count = 0;
    g_madt.gicr_base = 0;
    g_madt.gicr_length = 0;
    g_madt.msi_frame = {};

    uint32_t table_length = read_u32_safe(&table->length);
    const auto* base = reinterpret_cast<const uint8_t*>(m);
    const auto* ptr = base + sizeof(acpi::madt_table);
    const auto* end = base + table_length;

    while (ptr + sizeof(madt_entry_header) <= end) {
        const auto* entry = reinterpret_cast<const madt_entry_header*>(ptr);
        if (entry->length < 2 || ptr + entry->length > end) break;

        switch (entry->type) {
        case MADT_TYPE_GICC: {
            if (entry->length < GICC_MIN_LENGTH) break;
            const auto* e = reinterpret_cast<const madt_gicc*>(ptr);
            uint32_t flags = read_u32_safe(&e->flags);
            if (g_madt.cpu_count < MAX_CPUS) {
                auto& gc = g_madt.giccs[g_madt.cpu_count++];
                gc.base_address = read_u64_safe(&e->base_address);
                gc.mpidr = read_u64_safe(&e->arm_mpidr);
                gc.enabled = (flags & GICC_FLAG_ENABLED) != 0;
            }
            break;
        }
        case MADT_TYPE_GICD: {
            if (entry->length < sizeof(madt_gicd)) break;
            const auto* e = reinterpret_cast<const madt_gicd*>(ptr);
            g_madt.gicd_base = read_u64_safe(&e->base_address);
            g_madt.gic_version = e->version;
            break;
        }
        case MADT_TYPE_GIC_MSI_FRAME: {
            if (entry->length < sizeof(madt_gic_msi_frame)) {
                break;
            }
            if (g_madt.msi_frame.base_address != 0) {
                break;
            }
            const auto* e = reinterpret_cast<const madt_gic_msi_frame*>(ptr);
            g_madt.msi_frame.base_address = read_u64_safe(&e->base_address);
            g_madt.msi_frame.flags = read_u32_safe(&e->flags);
            uint32_t spi_field = read_u32_safe(&e->spi_count);
            g_madt.msi_frame.spi_count = static_cast<uint16_t>(spi_field & 0xFFFF);
            g_madt.msi_frame.spi_base = static_cast<uint16_t>(spi_field >> 16);
            break;
        }
        case MADT_TYPE_GICR: {
            if (entry->length < sizeof(madt_gicr)) break;
            const auto* e = reinterpret_cast<const madt_gicr*>(ptr);
            if (g_madt.gicr_base == 0) {
                g_madt.gicr_base = read_u64_safe(&e->base_address);
                g_madt.gicr_length = read_u32_safe(&e->length);
            }
            break;
        }
        default:
            break;
        }

        ptr += entry->length;
    }

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void dump() {
    log::info("madt: GIC version %u", static_cast<uint32_t>(g_madt.gic_version));
    log::info("madt: GICD base=0x%lx", g_madt.gicd_base);

    for (uint32_t i = 0; i < g_madt.cpu_count; i++) {
        const auto& gc = g_madt.giccs[i];
        log::info("madt: CPU %u: GICC base=0x%lx MPIDR=0x%lx (%s)",
                  i, gc.base_address, gc.mpidr,
                  gc.enabled ? "enabled" : "disabled");
    }

    if (g_madt.gicr_base != 0) {
        log::info("madt: GICR base=0x%lx length=0x%x",
                  g_madt.gicr_base, g_madt.gicr_length);
    }
}

} // namespace madt
} // namespace acpi
