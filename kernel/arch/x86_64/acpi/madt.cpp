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
    g_madt.lapic_base = read_u32_safe(&m->local_apic_address);
    g_madt.lapic_count = 0;
    g_madt.io_apic_count = 0;
    g_madt.iso_count = 0;
    g_madt.nmi_count = 0;

    uint32_t table_length = read_u32_safe(&table->length);
    const auto* base = reinterpret_cast<const uint8_t*>(m);
    const auto* ptr = base + sizeof(acpi::madt_table);
    const auto* end = base + table_length;

    while (ptr + sizeof(madt_entry_header) <= end) {
        const auto* entry = reinterpret_cast<const madt_entry_header*>(ptr);
        if (entry->length < 2 || ptr + entry->length > end) break;

        switch (entry->type) {
        case MADT_TYPE_LOCAL_APIC: {
            if (entry->length < sizeof(madt_local_apic)) break;
            const auto* e = reinterpret_cast<const madt_local_apic*>(ptr);
            uint32_t flags = read_u32_safe(&e->flags);
            if (g_madt.lapic_count < MAX_CPUS) {
                auto& l = g_madt.lapics[g_madt.lapic_count++];
                l.apic_id = e->apic_id;
                l.enabled = (flags & LAPIC_FLAG_ENABLED) != 0;
            }
            break;
        }
        case MADT_TYPE_IO_APIC: {
            if (entry->length < sizeof(madt_io_apic)) break;
            const auto* e = reinterpret_cast<const madt_io_apic*>(ptr);
            if (g_madt.io_apic_count < MAX_IO_APICS) {
                auto& io = g_madt.io_apics[g_madt.io_apic_count++];
                io.id = e->id;
                io.address = read_u32_safe(&e->address);
                io.global_irq_base = read_u32_safe(&e->global_irq_base);
            }
            break;
        }
        case MADT_TYPE_INT_SRC_OVERRIDE: {
            if (entry->length < sizeof(madt_int_src_override)) break;
            const auto* e = reinterpret_cast<const madt_int_src_override*>(ptr);
            if (g_madt.iso_count < MAX_ISOS) {
                auto& iso = g_madt.isos[g_madt.iso_count++];
                iso.bus = e->bus;
                iso.source = e->source;
                iso.global_irq = read_u32_safe(&e->global_irq);
                iso.flags = static_cast<uint16_t>(
                    read_u32_safe(&e->flags) & 0xFFFF);
            }
            break;
        }
        case MADT_TYPE_LOCAL_APIC_NMI: {
            if (entry->length < sizeof(madt_local_apic_nmi)) break;
            const auto* e = reinterpret_cast<const madt_local_apic_nmi*>(ptr);
            if (g_madt.nmi_count < MAX_NMIS) {
                auto& nmi = g_madt.nmis[g_madt.nmi_count++];
                nmi.processor_id = e->acpi_processor_id;
                uint32_t raw = read_u32_safe(&e->flags);
                nmi.flags = static_cast<uint16_t>(raw & 0xFFFF);
                nmi.lint = e->lint;
            }
            break;
        }
        case MADT_TYPE_LAPIC_ADDR_OVERRIDE: {
            if (entry->length < sizeof(madt_lapic_addr_override)) break;
            const auto* e = reinterpret_cast<const madt_lapic_addr_override*>(ptr);
            g_madt.lapic_base = read_u64_safe(&e->address);
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
    log::info("madt: LAPIC base=0x%lx", g_madt.lapic_base);

    for (uint32_t i = 0; i < g_madt.lapic_count; i++) {
        const auto& l = g_madt.lapics[i];
        log::info("madt: CPU %u: APIC ID=%u (%s)",
                  i, static_cast<uint32_t>(l.apic_id),
                  l.enabled ? "enabled" : "disabled");
    }

    for (uint32_t i = 0; i < g_madt.io_apic_count; i++) {
        const auto& io = g_madt.io_apics[i];
        log::info("madt: I/O APIC ID=%u addr=0x%08x GSI=%u",
                  static_cast<uint32_t>(io.id),
                  io.address,
                  io.global_irq_base);
    }

    for (uint32_t i = 0; i < g_madt.iso_count; i++) {
        const auto& iso = g_madt.isos[i];
        log::info("madt: ISO: IRQ %u -> GSI %u flags=0x%04x",
                  static_cast<uint32_t>(iso.source),
                  iso.global_irq,
                  static_cast<uint32_t>(iso.flags));
    }

    for (uint32_t i = 0; i < g_madt.nmi_count; i++) {
        const auto& nmi = g_madt.nmis[i];
        log::info("madt: NMI: processor=0x%02x LINT#%u flags=0x%04x",
                  static_cast<uint32_t>(nmi.processor_id),
                  static_cast<uint32_t>(nmi.lint),
                  static_cast<uint32_t>(nmi.flags));
    }
}

} // namespace madt
} // namespace acpi
