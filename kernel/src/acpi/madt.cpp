#include <acpi/madt.h>
#include <serial/serial.h>

namespace acpi {
madt g_madt;

madt& madt::get() {
    return g_madt;
}

void madt::init(acpi_sdt_header* acpi_madt_table) {
    madt_table* table = reinterpret_cast<madt_table*>(acpi_madt_table);

    // Print basic MADT table information
    serial::com1_printf("MADT Table:\n");
    serial::com1_printf("  LAPIC Address  : 0x%08x\n", table->lapic_address);
    serial::com1_printf("  Flags          : 0x%08x\n", table->flags);

    uint8_t* entry = table->entries;
    uint8_t* table_end = reinterpret_cast<uint8_t*>(table) + table->header.length;

    while (entry < table_end) {
        uint8_t entry_type = *entry;
        uint8_t entry_length = *(entry + 1);

        switch (entry_type) {
        case MADT_DESCRIPTOR_TYPE_LAPIC: {
            lapic_desc* desc = reinterpret_cast<lapic_desc*>(entry);
            m_local_apics.push_back(*desc);
            break;
        }
        case MADT_DESCRIPTOR_TYPE_IOAPIC: {
            // ioapic_desc* desc = reinterpret_cast<ioapic_desc*>(entry);

            // serial::com1_printf("IOAPIC Entry:\n");
            // serial::com1_printf("  IOAPIC ID: %u\n", desc->ioapic_id);
            // serial::com1_printf("  IOAPIC Address: 0x%08x\n", desc->ioapic_address);
            // serial::com1_printf("  Global System Interrupt Base: %u\n", desc->global_system_interrupt_base);
            break;
        }
        default: {
            break;
        }
        }

        // Advance to the next entry
        entry += entry_length;
    }
}
} // namespace acpi
