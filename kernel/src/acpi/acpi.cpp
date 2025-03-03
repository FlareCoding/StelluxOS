#include <acpi/acpi.h>
#include <memory/memory.h>
#include <memory/vmm.h>
#include <memory/paging.h>
#include <serial/serial.h>
#include <pci/pci_manager.h>
#include <acpi/hpet.h>
#include <acpi/madt.h>
#include <acpi/fadt.h>
#include <time/time.h>

extern char* g_mbi_kernel_cmdline;

void __detect_and_use_baremetal_pci_serial_controller() {
    auto& pci = pci::pci_manager::get();

    auto serial_controller = pci.find_by_class(PCI_CLASS_SIMPLE_COMMUNICATION_CONTROLLER, PCI_SUBCLASS_SIMPLE_COMM_SERIAL);
    if (!serial_controller.get()) {
        return;
    }

    serial_controller->enable();

    auto& bar = serial_controller->get_bars()[0];
    serial_controller->dbg_print_to_string();

    if (bar.type == pci::pci_bar_type::io_space) {
        uint16_t io_base = static_cast<uint16_t>(bar.address);

        serial::init_port(io_base, SERIAL_BAUD_DIVISOR_9600);
        serial::mark_serial_port_unprivileged(io_base);

        // If the GDB stub is enabled, don't direct normal kernel serial traffic
        // to the UART port, but rather leave it for the GDB stub to use.
        kstl::string cmdline_args = kstl::string(g_mbi_kernel_cmdline);
        if (cmdline_args.find("enable-gdb-stub") != kstl::string::npos) {
            serial::g_kernel_gdb_stub_uart_port = io_base;
        } else {
            serial::set_kernel_uart_port(io_base);
        }
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

/**
 * @brief Ensures that the ACPI table is fully mapped in memory.
 * 
 * This function checks if the ACPI table spans multiple pages and maps any additional
 * pages required to ensure the table is fully accessible in virtual memory.
 * 
 * @param table_address The physical address of the ACPI table.
 */
__PRIVILEGED_CODE
void map_acpi_table(uintptr_t table_address) {
    // Initially, just one page has to be mapped for the table
    if (paging::get_physical_address(reinterpret_cast<void*>(table_address)) == 0) {
        paging::map_page(table_address, table_address, DEFAULT_PRIV_PAGE_FLAGS, paging::get_pml4());
    }

    // Now that we can access the table's header, we can look at the length
    acpi_sdt_header* table = reinterpret_cast<acpi_sdt_header*>(table_address);

    // Calculate the starting and ending physical addresses
    uintptr_t start_address = table_address;
    uintptr_t end_address = table_address + table->length - 1;

    // Align start and end addresses to page boundaries
    uintptr_t start_page = start_address & ~(PAGE_SIZE - 1);
    uintptr_t end_page = end_address & ~(PAGE_SIZE - 1);

    // Traverse and map each page in the range
    for (uintptr_t current_page = start_page; current_page <= end_page; current_page += PAGE_SIZE) {
        // Check if the page is already mapped
        uintptr_t physical_address = paging::get_physical_address(reinterpret_cast<void*>(current_page));
        if (physical_address == 0) {
            // Map the page if it's not already mapped
            paging::map_page(current_page, current_page, DEFAULT_PRIV_PAGE_FLAGS, paging::get_pml4());
        }
    }
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

    // Ensure that the XSDT table is mapped into the kernel's address space
    map_acpi_table(rsdp_desc->xsdt_address);

    // Validate XSDT checksum
    if (!acpi_validate_checksum(xsdt_table, xsdt_table->header.length)) {
        serial::printf("[*] XSDT has an invalid checksum\n");
        return;
    }

    kstl::string cmdline_args = kstl::string(g_mbi_kernel_cmdline);
    bool use_pci_serial = (cmdline_args.find("use-pci-serial=true") != kstl::string::npos);

    // Enumerate tables
    size_t entry_count = (xsdt_table->header.length - sizeof(acpi_sdt_header)) / sizeof(uint64_t);
    for (size_t i = 0; i < entry_count; ++i) {
        auto table_address = xsdt_table->entries[i];
        map_acpi_table(table_address);

        acpi_sdt_header* table = reinterpret_cast<acpi_sdt_header*>(table_address);

        // Validate table checksum
        if (acpi_validate_checksum(table, table->length)) {
            char table_name[5] = { 0 };
            memcpy(table_name, table->signature, 4);
            
            // Handle specific table (e.g., MADT, FADT, etc.)
            if (strcmp(table_name, "MCFG") == 0) {
                // Initialize the PCI subsystem
                auto& pci = pci::pci_manager::get();
                pci.init(table);

                if (use_pci_serial) {
                    // For baremetal machines that have a PCI serial adapter card
                    // installed, configure it and set that port to be the primary
                    // kernel output UART port for increased baremetal debuggability.
                    __detect_and_use_baremetal_pci_serial_controller();
                }

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
            } else if (strcmp(table_name, "FACP") == 0) {
                // Initialize the FADT table controller
                auto& fadt_table = fadt::get();
                fadt_table.init(table);
            }
        }
    }
    serial::printf("\n");
}
} // namespace acpi
