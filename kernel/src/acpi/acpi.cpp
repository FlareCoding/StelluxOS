#include <acpi/acpi.h>
#include <memory/memory.h>
#include <memory/vmm.h>
#include <memory/paging.h>
#include <serial/serial.h>
#include <pci/pci_device.h>

// UART I/O Register Offsets
#define SERIAL_DATA_PORT_UART(base)               (base + 0)
#define SERIAL_INTERRUPT_ENABLE_PORT(base)   (base + 1)
#define SERIAL_FIFO_COMMAND_PORT(base)       (base + 2)
#define SERIAL_LINE_COMMAND_PORT(base)       (base + 3)
#define SERIAL_MODEM_COMMAND_PORT(base)      (base + 4)
#define SERIAL_LINE_STATUS_PORT(base)        (base + 5)

// UART Line Control Register (LCR) Flags
#define SERIAL_LCR_ENABLE_DLAB               0x80 // Enable Divisor Latch Access
#define SERIAL_LCR_8_BITS_NO_PARITY_ONE_STOP 0x03 // 8 bits, no parity, 1 stop bit

// UART FIFO Control Register (FCR) Flags
#define SERIAL_FCR_ENABLE_FIFO               0x01 // Enable FIFO
#define SERIAL_FCR_CLEAR_RECEIVE_FIFO        0x02 // Clear Receive FIFO
#define SERIAL_FCR_CLEAR_TRANSMIT_FIFO       0x04 // Clear Transmit FIFO
#define SERIAL_FCR_TRIGGER_14_BYTES          0xC0 // Set trigger level to 14 bytes

// UART Modem Control Register (MCR) Flags
#define SERIAL_MCR_RTS_DSR                   0x03 // Ready to Send (RTS), Data Set Ready (DSR)

// UART Line Status Register (LSR) Flags
#define SERIAL_LSR_TRANSMIT_EMPTY            0x20 // Transmitter Holding Register Empty
#define SERIAL_LSR_DATA_READY                0x01 // Data Ready

void init_port(uint16_t io_base) {
    // Disable all interrupts
    outb(SERIAL_INTERRUPT_ENABLE_PORT(io_base), 0x00);

    // Enable DLAB (Divisor Latch Access) to set baud rate
    outb(SERIAL_LINE_COMMAND_PORT(io_base), SERIAL_LCR_ENABLE_DLAB);

    // Set baud rate divisor to 0x000C (9600 baud at 1.8432 MHz clock)
    outb(SERIAL_DATA_PORT_UART(io_base), 0x0C); // Divisor Latch Low Byte
    outb(SERIAL_INTERRUPT_ENABLE_PORT(io_base), 0x00); // Divisor Latch High Byte

    // Configure line control: 8 bits, no parity, 1 stop bit
    outb(SERIAL_LINE_COMMAND_PORT(io_base), SERIAL_LCR_8_BITS_NO_PARITY_ONE_STOP);

    // Enable FIFO, clear TX/RX queues, set interrupt trigger level to 14 bytes
    outb(SERIAL_FIFO_COMMAND_PORT(io_base),
            SERIAL_FCR_ENABLE_FIFO |
            SERIAL_FCR_CLEAR_RECEIVE_FIFO |
            SERIAL_FCR_CLEAR_TRANSMIT_FIFO |
            SERIAL_FCR_TRIGGER_14_BYTES);

    // Set RTS/DSR
    outb(SERIAL_MODEM_COMMAND_PORT(io_base), SERIAL_MCR_RTS_DSR);
}

bool is_transmit_empty(uint16_t io_base) {
    return inb(SERIAL_LINE_STATUS_PORT(io_base)) & SERIAL_LSR_TRANSMIT_EMPTY;
}

void write_char(uint16_t io_base, char c) {
    // Wait until the Transmitter Holding Register is empty
    while (!is_transmit_empty(io_base));

    // Write the character to the Data Register
    outb(SERIAL_DATA_PORT_UART(io_base), c);
}

void write_string(uint16_t io_base, const char* str) {
    while (*str != '\0') {
        write_char(io_base, *str);
        str++;
    }
}

struct mcfg_entry {
    uint64_t base_address;      // Base address for this PCI segment
    uint16_t pci_segment_group; // Segment group number
    uint8_t start_bus;          // Starting bus number
    uint8_t end_bus;            // Ending bus number
    uint32_t reserved;          // Reserved, must be 0
} __attribute__((packed));

struct mcfg_table {
    acpi::acpi_sdt_header header;   // Standard ACPI table header
    uint64_t reserved;              // Reserved field
} __attribute__((packed));

uint64_t getBarFromPciHeader(pci::pci_function_desc* header, size_t barIndex) {
    uint32_t barValue = header->bar[barIndex];

    // Check if the BAR is memory-mapped
    if ((barValue & 0x1) == 0) {
        // Check if the BAR is 64-bit (by checking the type in bits [1:2])
        if ((barValue & 0x6) == 0x4) {
            // It's a 64-bit BAR, read the high part from the next BAR
            uint64_t high = (uint64_t)(header->bar[barIndex + 1]);
            uint64_t address = (high << 32) | (barValue & ~0xF);
            return address;
        } else {
            // It's a 32-bit BAR
            uint64_t address = (uint64_t)(barValue & ~0xF);
            return address;
        }
    }

    return 0; // No valid BAR found
}

void enumeratePciFunction(uint64_t deviceAddress, uint64_t function) {
    uint64_t function_offset = function << 12;

    uint64_t functionAddress = deviceAddress + function_offset;
    void* functionVirtualAddress = vmm::map_physical_page(functionAddress, DEFAULT_MAPPING_FLAGS);

    pci::pci_function_desc* pciDeviceHeader = (pci::pci_function_desc*)functionVirtualAddress;

    if (pciDeviceHeader->device_id == 0 || pciDeviceHeader->device_id == 0xffff) {
        vmm::unmap_virtual_page((uintptr_t)functionVirtualAddress);
        return;
    }

    pci::pci_device device(functionAddress, pciDeviceHeader);
    device.dbg_print_to_string();

    if (pciDeviceHeader->class_code == PCI_CLASS_SIMPLE_COMMUNICATION_CONTROLLER &&
        pciDeviceHeader->subclass == PCI_SUBCLASS_SIMPLE_COMM_SERIAL &&
        pciDeviceHeader->prog_if == PCI_PROGIF_SERIAL_16550_COMPATIBLE    
    ) {
        auto& bars = device.get_bars();
        auto& bar0 = bars[0];

        device.enable();

        // Decode BAR0 as I/O base
        if (bar0.type == pci::pci_bar_type::io_space) { // I/O space
            uint16_t io_base = bar0.address & ~0x3;

            init_port(io_base);
            write_string(io_base, "UART messages work!\n");
            write_string(io_base, "Welcome to Stellux 2.0!\n");
        }
    }
}

void enumeratePciDevice(uint64_t busAddress, uint64_t device) {
    uint64_t device_offset = device << 15;
    uint64_t deviceAddress = busAddress + device_offset;

    void* deviceVirtualAddress = vmm::map_physical_page(deviceAddress, DEFAULT_MAPPING_FLAGS);

    pci::pci_function_desc* pciDeviceHeader = (pci::pci_function_desc*)deviceVirtualAddress;

    if (pciDeviceHeader->device_id == 0 || pciDeviceHeader->device_id == 0xffff) {
        vmm::unmap_virtual_page((uintptr_t)deviceVirtualAddress);
        return;
    }

    for (uint64_t function = 0; function < 8; function++){
        enumeratePciFunction(deviceAddress, function);
    }
}

void enumeratePciBus(uint64_t baseAddress, uint64_t bus) {
    uint64_t bus_offset = bus << 20;
    uint64_t busAddress = baseAddress + bus_offset;

    void* busVirtualAddress = vmm::map_physical_page(busAddress, DEFAULT_MAPPING_FLAGS);

    pci::pci_function_desc* pciDeviceHeader = (pci::pci_function_desc*)busVirtualAddress;

    if (pciDeviceHeader->device_id == 0 || pciDeviceHeader->device_id == 0xffff) {
        vmm::unmap_virtual_page((uintptr_t)busVirtualAddress);
        return;
    }

    for (uint64_t device = 0; device < 32; device++){
        enumeratePciDevice(busAddress, device);
    }
}

void enumerate_mcfg_table(mcfg_table* mcfg) {
    size_t entries = (mcfg->header.length - sizeof(mcfg_table)) / sizeof(mcfg_entry);
    for (size_t t = 0; t < entries; t++) {
        mcfg_entry* device_config = (mcfg_entry*)((uint64_t)mcfg + sizeof(mcfg_table) + (sizeof(mcfg_entry) * t));
        for (uint64_t bus = device_config->start_bus; bus < device_config->end_bus; bus++) {
            enumeratePciBus(device_config->base_address, bus);
        }
    }
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
        serial::com1_printf("[*] RSDP was null\n");
        return;
    }

    // Get XSDT address
    rsdp_descriptor* rsdp_desc = reinterpret_cast<rsdp_descriptor*>(rsdp);
    xsdt* xsdt_table = reinterpret_cast<xsdt*>(rsdp_desc->xsdt_address);

    // Validate XSDT checksum
    if (!acpi_validate_checksum(xsdt_table, xsdt_table->header.length)) {
        serial::com1_printf("[*] XSDT has an invalid checksum\n");
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
            serial::com1_printf("Found ACPI table: %s (0x%llx)\n", table_name, table);

            if (strcmp(table_name, "MCFG") == 0) {
                mcfg_table* mcfg = reinterpret_cast<mcfg_table*>(table);
                enumerate_mcfg_table(mcfg);
            }
        }
    }
    serial::com1_printf("\n");
}
} // namespace acpi
