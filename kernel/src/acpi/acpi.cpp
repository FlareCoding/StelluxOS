#include <acpi/acpi.h>
#include <memory/memory.h>
#include <memory/vmm.h>
#include <memory/paging.h>
#include <serial/serial.h>
#include <pci/pci_manager.h>
#include <acpi/hpet.h>

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

void pci_test() {
    auto& pci = pci::pci_manager::get();

    for (auto& device : pci.get_devices()) {
        device->dbg_print_to_string();
    }

    auto serial_controller = pci.find_by_class(PCI_CLASS_SIMPLE_COMMUNICATION_CONTROLLER, PCI_SUBCLASS_SIMPLE_COMM_SERIAL);
    if (!serial_controller.get()) {
        return;
    }

    serial::com1_printf("\n   [*] Found serial controller!\n");
    serial_controller->dbg_print_to_string();

    serial_controller->enable();

    auto& bar = serial_controller->get_bars()[0];

    if (bar.type == pci::pci_bar_type::io_space) { // I/O space
        uint16_t io_base = bar.address & ~0x3;

        init_port(io_base);
        write_string(io_base, "UART messages work!\n");
        write_string(io_base, "Welcome to Stellux 2.0!\n");
    }
    serial::com1_printf("\n");
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
                // Initialize the PCI subsystem
                auto& pci = pci::pci_manager::get();
                pci.init(table);

                pci_test();
            } else if (strcmp(table_name, "HPET") == 0) {
                // Initialize the HPET timer
                auto& timer = hpet::get();
                timer.init(table);
            }
        }
    }
    serial::com1_printf("\n");
}
} // namespace acpi
