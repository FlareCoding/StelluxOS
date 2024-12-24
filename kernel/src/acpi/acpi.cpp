#include <acpi/acpi.h>
#include <memory/memory.h>
#include <memory/vmm.h>
#include <memory/paging.h>
#include <serial/serial.h>
#include <pci/pci_manager.h>
#include <acpi/hpet.h>
#include <acpi/madt.h>
#include <time/time.h>

void __detect_and_use_baremetal_pci_serial_controller() {
    auto& pci = pci::pci_manager::get();

    auto serial_controller = pci.find_by_class(PCI_CLASS_SIMPLE_COMMUNICATION_CONTROLLER, PCI_SUBCLASS_SIMPLE_COMM_SERIAL);
    if (!serial_controller.get()) {
        return;
    }

    serial_controller->enable();

    auto& bar = serial_controller->get_bars()[0];

    if (bar.type == pci::pci_bar_type::io_space) {
        uint16_t io_base = static_cast<uint16_t>(bar.address);

        serial::init_port(io_base, SERIAL_BAUD_DIVISOR_9600);
        serial::mark_serial_port_unprivileged(io_base);
        serial::set_kernel_uart_port(io_base);
    }
    
    for (auto& device : pci.get_devices()) {
        device->dbg_print_to_string();
    }

    serial::printf("\n");
}

namespace acpi {
__PRIVILEGED_CODE
bool acpi_validate_checksum(void* table, size_t length) {
    uint8_t sum = 0;
    uint8_t* data = reinterpret_cast<uint8_t*>(table);
    for (size_t i = 0; i < length; ++i) {
        sum += data[i];
    }
    return sum == 0;
}

__PRIVILEGED_CODE
void enumerate_acpi_tables(void* rsdp) {
    if (!rsdp) {
        serial::printf("[*] RSDP was null\n");
        return;
    }

    // Get XSDT address
    rsdp_descriptor* rsdp_desc = reinterpret_cast<rsdp_descriptor*>(rsdp);
    xsdt* xsdt_table = reinterpret_cast<xsdt*>(rsdp_desc->xsdt_address);

    // Validate XSDT checksum
    if (!acpi_validate_checksum(xsdt_table, xsdt_table->header.length)) {
        serial::printf("[*] XSDT has an invalid checksum\n");
        return;
    }

    // Enumerate tables
    size_t entry_count = (xsdt_table->header.length - sizeof(acpi_sdt_header)) / sizeof(uint64_t);
    for (size_t i = 0; i < entry_count; ++i) {
        auto table_address = xsdt_table->entries[i];
        acpi_sdt_header* table = reinterpret_cast<acpi_sdt_header*>(table_address);

        // Validate table checksum
        if (acpi_validate_checksum(table, table->length)) {
            char table_name[5] = { 0 };
            memcpy(table_name, table->signature, 4);
            
            // Handle specific table (e.g., MADT, FADT, etc.)
            serial::printf("Found ACPI table: %s (0x%llx)\n", table_name, table);

            if (strcmp(table_name, "MCFG") == 0) {
                // Initialize the PCI subsystem
                auto& pci = pci::pci_manager::get();
                pci.init(table);

                // For baremetal machines that have a PCI serial adapter card
                // installed, configure it and set that port to be the primary
                // kernel output UART port for increased baremetal debuggability.
                __detect_and_use_baremetal_pci_serial_controller();

            } else if (strcmp(table_name, "HPET") == 0) {
                // Initialize the HPET timer
                auto& timer = hpet::get();
                timer.init(table);

                // Initialize kernel time
                kernel_timer::init();
            } else if (strcmp(table_name, "APIC") == 0) {
                // Initialize the MADT table
                auto& apic_table = madt::get();
                apic_table.init(table);
            }
        }
    }
    serial::printf("\n");
}
} // namespace acpi
