#include <acpi/acpi.h>
#include <memory/memory.h>
#include <memory/vmm.h>
#include <memory/paging.h>
#include <serial/serial.h>
#include <pci/pci_class_codes.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

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

uint32_t pci_get_address(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    return (1U << 31) | // Enable bit
           (bus << 16) | // Bus number
           (device << 11) | // Device number
           (function << 8) | // Function number
           (offset & 0xFC); // Register offset (aligned to 4 bytes)
}

uint16_t pci_read_config_word(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t address = pci_get_address(bus, device, function, offset);
    outl(PCI_CONFIG_ADDRESS, address); // Write address to 0xCF8
    return inw(PCI_CONFIG_DATA + (offset & 2)); // Read 16 bits from 0xCFC
}

void pci_write_config_word(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value) {
    uint32_t address = pci_get_address(bus, device, function, offset);
    outl(PCI_CONFIG_ADDRESS, address); // Write address to 0xCF8
    outw(PCI_CONFIG_DATA + (offset & 2), value); // Write 16 bits to 0xCFC
}

struct pci_device_header {
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t command;
    uint16_t status;
    uint8_t revision_id;
    uint8_t prog_if;
    uint8_t subclass;
    uint8_t class_code;
    uint8_t cache_line_size;
    uint8_t latency_timer;
    uint8_t header_type;
    uint8_t bist;
    uint32_t bar[6];
    uint32_t cardbus_cisptr;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_id;
    uint32_t expansion_rombase_addr;
    uint8_t capabilities_ptr;
    uint8_t reserved[7];
    uint8_t interrupt_line;
    uint8_t interrupt_pin;
    uint8_t min_grant;
    uint8_t max_latency;
};

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

const char* get_pci_device_name(const pci_device_header* header) {
    switch (header->class_code) {
        case PCI_CLASS_UNCLASSIFIED:
            switch (header->subclass) {
                case PCI_SUBCLASS_UNCLASSIFIED_NON_VGA_COMPATIBLE:
                    return "Unclassified Non-VGA-Compatible Device";
                default:
                    return "Unclassified Device";
            }
        
        case PCI_CLASS_MASS_STORAGE_CONTROLLER:
            switch (header->subclass) {
                case PCI_SUBCLASS_MASS_STORAGE_SCSI:
                    return "Mass Storage - SCSI Controller";
                case PCI_SUBCLASS_MASS_STORAGE_IDE:
                    switch (header->prog_if) {
                        case PCI_PROGIF_IDE_ISA_COMPATIBILITY_MODE_ONLY:
                            return "Mass Storage - IDE Controller (ISA Compatibility Mode Only)";
                        case PCI_PROGIF_IDE_PCI_NATIVE_MODE_ONLY:
                            return "Mass Storage - IDE Controller (PCI Native Mode Only)";
                        case PCI_PROGIF_IDE_ISA_PCI_NATIVE_SUPPORT_BUS_MASTERING:
                            return "Mass Storage - IDE Controller (ISA + PCI Native Support, Bus Mastering)";
                        case PCI_PROGIF_IDE_PCI_ISA_NATIVE_SUPPORT_BUS_MASTERING:
                            return "Mass Storage - IDE Controller (PCI + ISA Native Support, Bus Mastering)";
                        default:
                            return "Mass Storage - IDE Controller (Unknown Prog IF)";
                    }
                case PCI_SUBCLASS_MASS_STORAGE_FLOPPY:
                    return "Mass Storage - Floppy Disk Controller";
                case PCI_SUBCLASS_MASS_STORAGE_IPI:
                    return "Mass Storage - IPI Bus Controller";
                case PCI_SUBCLASS_MASS_STORAGE_RAID:
                    return "Mass Storage - RAID Controller";
                case PCI_SUBCLASS_MASS_STORAGE_ATA:
                    switch (header->prog_if) {
                        case PCI_PROGIF_ATA_SINGLE_DMA:
                            return "Mass Storage - ATA Controller (Single DMA)";
                        case PCI_PROGIF_ATA_CHAINED_DMA:
                            return "Mass Storage - ATA Controller (Chained DMA)";
                        default:
                            return "Mass Storage - ATA Controller (Unknown Prog IF)";
                    }
                case PCI_SUBCLASS_MASS_STORAGE_SATA:
                    switch (header->prog_if) {
                        case PCI_PROGIF_SATA_VENDOR_SPECIFIC:
                            return "Mass Storage - Serial ATA Controller (Vendor Specific)";
                        case PCI_PROGIF_SATA_AHCI_1_0:
                            return "Mass Storage - Serial ATA Controller (AHCI 1.0)";
                        case PCI_PROGIF_SATA_SERIAL_STORAGE_BUS:
                            return "Mass Storage - Serial ATA Controller (Serial Storage Bus)";
                        default:
                            return "Mass Storage - Serial ATA Controller (Unknown Prog IF)";
                    }
                case PCI_SUBCLASS_MASS_STORAGE_SAS:
                    switch (header->prog_if) {
                        case PCI_PROGIF_SAS_SAS:
                            return "Mass Storage - Serial Attached SCSI Controller (SAS)";
                        case PCI_PROGIF_SAS_SERIAL_STORAGE_BUS:
                            return "Mass Storage - Serial Attached SCSI Controller (Serial Storage Bus)";
                        default:
                            return "Mass Storage - Serial Attached SCSI Controller (Unknown Prog IF)";
                    }
                case PCI_SUBCLASS_MASS_STORAGE_NVME:
                    switch (header->prog_if) {
                        case PCI_PROGIF_NVME_NVMHCI:
                            return "Mass Storage - Non-Volatile Memory Controller (NVMHCI)";
                        case PCI_PROGIF_NVME_NVM_EXPRESS:
                            return "Mass Storage - Non-Volatile Memory Controller (NVM Express)";
                        default:
                            return "Mass Storage - Non-Volatile Memory Controller (Unknown Prog IF)";
                    }
                case PCI_SUBCLASS_MASS_STORAGE_OTHER:
                    return "Mass Storage - Other Controller";
                default:
                    return "Mass Storage - Unknown Subclass";
            }

        case PCI_CLASS_NETWORK_CONTROLLER:
            switch (header->subclass) {
                case PCI_SUBCLASS_NETWORK_ETHERNET:
                    return "Network Controller - Ethernet Controller";
                case PCI_SUBCLASS_NETWORK_TOKEN_RING:
                    return "Network Controller - Token Ring Controller";
                case PCI_SUBCLASS_NETWORK_FDDI:
                    return "Network Controller - FDDI Controller";
                case PCI_SUBCLASS_NETWORK_ATM:
                    return "Network Controller - ATM Controller";
                case PCI_SUBCLASS_NETWORK_ISDN:
                    return "Network Controller - ISDN Controller";
                case PCI_SUBCLASS_NETWORK_WORLDFIP:
                    return "Network Controller - WorldFip Controller";
                case PCI_SUBCLASS_NETWORK_PICMG_2_14:
                    return "Network Controller - PICMG 2.14 Multi Computing Controller";
                case PCI_SUBCLASS_NETWORK_INFINIBAND:
                    return "Network Controller - Infiniband Controller";
                case PCI_SUBCLASS_NETWORK_FABRIC:
                    return "Network Controller - Fabric Controller";
                case PCI_SUBCLASS_NETWORK_OTHER:
                    return "Network Controller - Other Controller";
                default:
                    return "Network Controller - Unknown Subclass";
            }

        case PCI_CLASS_DISPLAY_CONTROLLER:
            switch (header->subclass) {
                case PCI_SUBCLASS_DISPLAY_VGA_COMPATIBLE:
                    switch (header->prog_if) {
                        case PCI_PROGIF_VGA_CONTROLLER:
                            return "Display Controller - VGA Compatible Controller";
                        case PCI_PROGIF_XGA_CONTROLLER:
                            return "Display Controller - XGA Controller";
                        case PCI_PROGIF_3D_CONTROLLER:
                            return "Display Controller - 3D Controller";
                        default:
                            return "Display Controller - VGA Compatible Controller (Unknown Prog IF)";
                    }
                case PCI_SUBCLASS_DISPLAY_OTHER:
                    return "Display Controller - Other Display Controller";
                default:
                    return "Display Controller - Unknown Subclass";
            }

        case PCI_CLASS_MULTIMEDIA_CONTROLLER:
            switch (header->subclass) {
                case PCI_SUBCLASS_MULTIMEDIA_VIDEO:
                    return "Multimedia Controller - Video Controller";
                case PCI_SUBCLASS_MULTIMEDIA_AUDIO:
                    return "Multimedia Controller - Audio Controller";
                case PCI_SUBCLASS_MULTIMEDIA_TELEPHONY:
                    return "Multimedia Controller - Computer Telephony Device";
                case PCI_SUBCLASS_MULTIMEDIA_AUDIO_DEVICE:
                    return "Multimedia Controller - Audio Device";
                case PCI_SUBCLASS_MULTIMEDIA_OTHER:
                    return "Multimedia Controller - Other Controller";
                default:
                    return "Multimedia Controller - Unknown Subclass";
            }

        case PCI_CLASS_MEMORY_CONTROLLER:
            switch (header->subclass) {
                case PCI_SUBCLASS_MEMORY_RAM:
                    return "Memory Controller - RAM Controller";
                case PCI_SUBCLASS_MEMORY_FLASH:
                    return "Memory Controller - Flash Controller";
                case PCI_SUBCLASS_MEMORY_OTHER:
                    return "Memory Controller - Other Memory Controller";
                default:
                    return "Memory Controller - Unknown Subclass";
            }

        case PCI_CLASS_BRIDGE:
            switch (header->subclass) {
                case PCI_SUBCLASS_BRIDGE_HOST:
                    return "Bridge - Host Bridge";
                case PCI_SUBCLASS_BRIDGE_ISA:
                    return "Bridge - ISA Bridge";
                case PCI_SUBCLASS_BRIDGE_EISA:
                    return "Bridge - EISA Bridge";
                case PCI_SUBCLASS_BRIDGE_MCA:
                    return "Bridge - MCA Bridge";
                case PCI_SUBCLASS_BRIDGE_PCI_TO_PCI:
                    switch (header->prog_if) {
                        case PCI_PROGIF_PCI_TO_PCI_NORMAL_DECODE:
                            return "Bridge - PCI-to-PCI Bridge (Normal Decode)";
                        case PCI_PROGIF_PCI_TO_PCI_SUBTRACTIVE_DECODE:
                            return "Bridge - PCI-to-PCI Bridge (Subtractive Decode)";
                        default:
                            return "Bridge - PCI-to-PCI Bridge (Unknown Prog IF)";
                    }
                case PCI_SUBCLASS_BRIDGE_PCMCIA:
                    return "Bridge - PCMCIA Bridge";
                case PCI_SUBCLASS_BRIDGE_NUBUS:
                    return "Bridge - NuBus Bridge";
                case PCI_SUBCLASS_BRIDGE_CARDBUS:
                    return "Bridge - CardBus Bridge";
                case PCI_SUBCLASS_BRIDGE_RACEWAY:
                    switch (header->prog_if) {
                        case PCI_PROGIF_RACEWAY_TRANSPARENT_MODE:
                            return "Bridge - RACEway Bridge (Transparent Mode)";
                        case PCI_PROGIF_RACEWAY_ENDPOINT_MODE:
                            return "Bridge - RACEway Bridge (Endpoint Mode)";
                        default:
                            return "Bridge - RACEway Bridge (Unknown Prog IF)";
                    }
                case PCI_SUBCLASS_BRIDGE_PCI_TO_PCI_ADDITIONAL:
                    switch (header->prog_if) {
                        case PCI_PROGIF_PCI_TO_PCI_SEMI_TRANSPARENT_PRIMARY:
                            return "Bridge - Additional PCI-to-PCI Bridge (Semi-Transparent Primary)";
                        case PCI_PROGIF_PCI_TO_PCI_SEMI_TRANSPARENT_SECONDARY:
                            return "Bridge - Additional PCI-to-PCI Bridge (Semi-Transparent Secondary)";
                        default:
                            return "Bridge - Additional PCI-to-PCI Bridge (Unknown Prog IF)";
                    }
                case PCI_SUBCLASS_BRIDGE_INFINIBAND_TO_PCI:
                    return "Bridge - InfiniBand-to-PCI Host Bridge";
                case PCI_SUBCLASS_BRIDGE_OTHER:
                    return "Bridge - Other Bridge";
                default:
                    return "Bridge - Unknown Subclass";
            }

        case PCI_CLASS_SIMPLE_COMMUNICATION_CONTROLLER:
            switch (header->subclass) {
                case PCI_SUBCLASS_SIMPLE_COMM_SERIAL:
                    switch (header->prog_if) {
                        case PCI_PROGIF_SERIAL_8250_COMPATIBLE:
                            return "Simple Communication Controller - Serial Controller (8250 Compatible)";
                        case PCI_PROGIF_SERIAL_16450_COMPATIBLE:
                            return "Simple Communication Controller - Serial Controller (16450 Compatible)";
                        case PCI_PROGIF_SERIAL_16550_COMPATIBLE:
                            return "Simple Communication Controller - Serial Controller (16550 Compatible)";
                        case PCI_PROGIF_SERIAL_16650_COMPATIBLE:
                            return "Simple Communication Controller - Serial Controller (16650 Compatible)";
                        case PCI_PROGIF_SERIAL_16750_COMPATIBLE:
                            return "Simple Communication Controller - Serial Controller (16750 Compatible)";
                        case PCI_PROGIF_SERIAL_16850_COMPATIBLE:
                            return "Simple Communication Controller - Serial Controller (16850 Compatible)";
                        case PCI_PROGIF_SERIAL_16950_COMPATIBLE:
                            return "Simple Communication Controller - Serial Controller (16950 Compatible)";
                        default:
                            return "Simple Communication Controller - Serial Controller (Unknown Prog IF)";
                    }
                case PCI_SUBCLASS_SIMPLE_COMM_PARALLEL:
                    switch (header->prog_if) {
                        case PCI_PROGIF_PARALLEL_STANDARD_PORT:
                            return "Simple Communication Controller - Parallel Controller (Standard Port)";
                        case PCI_PROGIF_PARALLEL_BIDIRECTIONAL_PORT:
                            return "Simple Communication Controller - Parallel Controller (Bidirectional Port)";
                        case PCI_PROGIF_PARALLEL_ECP_1_X:
                            return "Simple Communication Controller - Parallel Controller (ECP 1.x)";
                        case PCI_PROGIF_PARALLEL_IEEE_1284_CONTROLLER:
                            return "Simple Communication Controller - Parallel Controller (IEEE 1284 Controller)";
                        case PCI_PROGIF_PARALLEL_IEEE_1284_TARGET_DEVICE:
                            return "Simple Communication Controller - Parallel Controller (IEEE 1284 Target Device)";
                        default:
                            return "Simple Communication Controller - Parallel Controller (Unknown Prog IF)";
                    }
                case PCI_SUBCLASS_SIMPLE_COMM_MULTIPORT_SERIAL:
                    return "Simple Communication Controller - Multiport Serial Controller";
                case PCI_SUBCLASS_SIMPLE_COMM_MODEM:
                    switch (header->prog_if) {
                        case PCI_PROGIF_MODEM_GENERIC:
                            return "Simple Communication Controller - Modem (Generic)";
                        case PCI_PROGIF_MODEM_HAYES_16450_COMPATIBLE:
                            return "Simple Communication Controller - Modem (Hayes 16450 Compatible)";
                        case PCI_PROGIF_MODEM_HAYES_16550_COMPATIBLE:
                            return "Simple Communication Controller - Modem (Hayes 16550 Compatible)";
                        case PCI_PROGIF_MODEM_HAYES_16650_COMPATIBLE:
                            return "Simple Communication Controller - Modem (Hayes 16650 Compatible)";
                        case PCI_PROGIF_MODEM_HAYES_16750_COMPATIBLE:
                            return "Simple Communication Controller - Modem (Hayes 16750 Compatible)";
                        default:
                            return "Simple Communication Controller - Modem (Unknown Prog IF)";
                    }
                case PCI_SUBCLASS_SIMPLE_COMM_GPIP:
                    return "Simple Communication Controller - IEEE 488.1/2 (GPIB) Controller";
                case PCI_SUBCLASS_SIMPLE_COMM_SMART_CARD:
                    return "Simple Communication Controller - Smart Card Controller";
                case PCI_SUBCLASS_SIMPLE_COMM_OTHER:
                    return "Simple Communication Controller - Other Controller";
                default:
                    return "Simple Communication Controller - Unknown Subclass";
            }

        case PCI_CLASS_BASE_SYSTEM_PERIPHERAL:
            switch (header->subclass) {
                case PCI_SUBCLASS_BASE_SYSTEM_PIC:
                    switch (header->prog_if) {
                        case PCI_PROGIF_PIC_GENERIC_8259_COMPATIBLE:
                            return "Base System Peripheral - PIC (Generic 8259 Compatible)";
                        case PCI_PROGIF_PIC_ISA_COMPATIBLE:
                            return "Base System Peripheral - PIC (ISA Compatible)";
                        case PCI_PROGIF_PIC_EISA_COMPATIBLE:
                            return "Base System Peripheral - PIC (EISA Compatible)";
                        case PCI_PROGIF_PIC_IO_APIC_INTERRUPT_CONTROLLER:
                            return "Base System Peripheral - PIC (IO-APIC Interrupt Controller)";
                        case PCI_PROGIF_PIC_IO_X_APIC_INTERRUPT_CONTROLLER:
                            return "Base System Peripheral - PIC (IO-X APIC Interrupt Controller)";
                        default:
                            return "Base System Peripheral - PIC (Unknown Prog IF)";
                    }
                case PCI_SUBCLASS_BASE_SYSTEM_DMA:
                    switch (header->prog_if) {
                        case PCI_PROGIF_DMA_GENERIC_8237_COMPATIBLE:
                            return "Base System Peripheral - DMA Controller (Generic 8237 Compatible)";
                        case PCI_PROGIF_DMA_ISA_COMPATIBLE:
                            return "Base System Peripheral - DMA Controller (ISA Compatible)";
                        case PCI_PROGIF_DMA_EISA_COMPATIBLE:
                            return "Base System Peripheral - DMA Controller (EISA Compatible)";
                        default:
                            return "Base System Peripheral - DMA Controller (Unknown Prog IF)";
                    }
                case PCI_SUBCLASS_BASE_SYSTEM_TIMER:
                    switch (header->prog_if) {
                        case PCI_PROGIF_TIMER_GENERIC_8254_COMPATIBLE:
                            return "Base System Peripheral - Timer (Generic 8254 Compatible)";
                        case PCI_PROGIF_TIMER_ISA_COMPATIBLE:
                            return "Base System Peripheral - Timer (ISA Compatible)";
                        case PCI_PROGIF_TIMER_EISA_COMPATIBLE:
                            return "Base System Peripheral - Timer (EISA Compatible)";
                        case PCI_PROGIF_TIMER_HPET:
                            return "Base System Peripheral - Timer (HPET)";
                        default:
                            return "Base System Peripheral - Timer (Unknown Prog IF)";
                    }
                case PCI_SUBCLASS_BASE_SYSTEM_RTC:
                    switch (header->prog_if) {
                        case PCI_PROGIF_RTC_GENERIC:
                            return "Base System Peripheral - RTC Controller (Generic)";
                        case PCI_PROGIF_RTC_ISA_COMPATIBLE:
                            return "Base System Peripheral - RTC Controller (ISA Compatible)";
                        default:
                            return "Base System Peripheral - RTC Controller (Unknown Prog IF)";
                    }
                case PCI_SUBCLASS_BASE_SYSTEM_PCI_HOT_PLUG:
                    return "Base System Peripheral - PCI Hot-Plug Controller";
                case PCI_SUBCLASS_BASE_SYSTEM_SD_HOST:
                    return "Base System Peripheral - SD Host Controller";
                case PCI_SUBCLASS_BASE_SYSTEM_IOMMU:
                    return "Base System Peripheral - IOMMU";
                case PCI_SUBCLASS_BASE_SYSTEM_OTHER:
                    return "Base System Peripheral - Other Peripheral";
                default:
                    return "Base System Peripheral - Unknown Subclass";
            }

        case PCI_CLASS_INPUT_DEVICE_CONTROLLER:
            switch (header->subclass) {
                case PCI_SUBCLASS_INPUT_KEYBOARD:
                    return "Input Device Controller - Keyboard Controller";
                case PCI_SUBCLASS_INPUT_DIGITIZER_PEN:
                    return "Input Device Controller - Digitizer Pen";
                case PCI_SUBCLASS_INPUT_MOUSE:
                    return "Input Device Controller - Mouse Controller";
                case PCI_SUBCLASS_INPUT_SCANNER:
                    return "Input Device Controller - Scanner Controller";
                case PCI_SUBCLASS_INPUT_GAMEPORT:
                    switch (header->prog_if) {
                        case PCI_PROGIF_GAMEPORT_GENERIC:
                            return "Input Device Controller - Gameport Controller (Generic)";
                        case PCI_PROGIF_GAMEPORT_EXTENDED:
                            return "Input Device Controller - Gameport Controller (Extended)";
                        default:
                            return "Input Device Controller - Gameport Controller (Unknown Prog IF)";
                    }
                case PCI_SUBCLASS_INPUT_OTHER:
                    return "Input Device Controller - Other Controller";
                default:
                    return "Input Device Controller - Unknown Subclass";
            }

        case PCI_CLASS_DOCKING_STATION:
            switch (header->subclass) {
                case PCI_SUBCLASS_DOCKING_GENERIC:
                    return "Docking Station - Generic Docking Station";
                case PCI_SUBCLASS_DOCKING_OTHER:
                    return "Docking Station - Other Docking Station";
                default:
                    return "Docking Station - Unknown Subclass";
            }

        case PCI_CLASS_PROCESSOR:
            switch (header->subclass) {
                case PCI_SUBCLASS_PROCESSOR_386:
                    return "Processor - 386 Processor";
                case PCI_SUBCLASS_PROCESSOR_486:
                    return "Processor - 486 Processor";
                case PCI_SUBCLASS_PROCESSOR_PENTIUM:
                    return "Processor - Pentium Processor";
                case PCI_SUBCLASS_PROCESSOR_PENTIUM_PRO:
                    return "Processor - Pentium Pro Processor";
                case PCI_SUBCLASS_PROCESSOR_ALPHA:
                    return "Processor - Alpha Processor";
                case PCI_SUBCLASS_PROCESSOR_POWERPC:
                    return "Processor - PowerPC Processor";
                case PCI_SUBCLASS_PROCESSOR_MIPS:
                    return "Processor - MIPS Processor";
                case PCI_SUBCLASS_PROCESSOR_CO_PROCESSOR:
                    return "Processor - Co-Processor";
                case PCI_SUBCLASS_PROCESSOR_OTHER:
                    return "Processor - Other Processor";
                default:
                    return "Processor - Unknown Subclass";
            }

        case PCI_CLASS_SERIAL_BUS_CONTROLLER:
            switch (header->subclass) {
                case PCI_SUBCLASS_SERIAL_BUS_FIREWIRE:
                    switch (header->prog_if) {
                        case PCI_PROGIF_FIREWIRE_GENERIC:
                            return "Serial Bus Controller - FireWire (IEEE 1394) Controller (Generic)";
                        case PCI_PROGIF_FIREWIRE_OHCI:
                            return "Serial Bus Controller - FireWire (IEEE 1394) Controller (OHCI)";
                        case PCI_PROGIF_FIREWIRE_ACCESS_BUS_CONTROLLER:
                            return "Serial Bus Controller - FireWire (IEEE 1394) Controller (Access Bus Controller)";
                        case PCI_PROGIF_FIREWIRE_SSA:
                            return "Serial Bus Controller - FireWire (IEEE 1394) Controller (SSA)";
                        default:
                            return "Serial Bus Controller - FireWire (IEEE 1394) Controller (Unknown Prog IF)";
                    }
                case PCI_SUBCLASS_SERIAL_BUS_USB:
                    switch (header->prog_if) {
                        case PCI_PROGIF_USB_UHCI:
                            return "Serial Bus Controller - USB Controller (UHCI)";
                        case PCI_PROGIF_USB_OHCI:
                            return "Serial Bus Controller - USB Controller (OHCI)";
                        case PCI_PROGIF_USB_EHCI:
                            return "Serial Bus Controller - USB Controller (EHCI)";
                        case PCI_PROGIF_USB_XHCI:
                            return "Serial Bus Controller - USB Controller (XHCI)";
                        case PCI_PROGIF_USB_UNSPECIFIED:
                            return "Serial Bus Controller - USB Controller (Unspecified)";
                        case PCI_PROGIF_USB_DEVICE_NOT_HOST_CONTROLLER:
                            return "Serial Bus Controller - USB Controller (Device Not Host Controller)";
                        default:
                            return "Serial Bus Controller - USB Controller (Unknown Prog IF)";
                    }
                case PCI_SUBCLASS_SERIAL_BUS_FIBRE_CHANNEL:
                    return "Serial Bus Controller - Fibre Channel";
                case PCI_SUBCLASS_SERIAL_BUS_SMBUS:
                    return "Serial Bus Controller - SMBus Controller";
                case PCI_SUBCLASS_SERIAL_BUS_INFINIBAND:
                    return "Serial Bus Controller - InfiniBand Controller";
                case PCI_SUBCLASS_SERIAL_BUS_IPMI:
                    switch (header->prog_if) {
                        case PCI_PROGIF_IPMI_SMIC:
                            return "Serial Bus Controller - IPMI Interface (SMIC)";
                        case PCI_PROGIF_IPMI_KEYBOARD_CONTROLLER_STYLE:
                            return "Serial Bus Controller - IPMI Interface (Keyboard Controller Style)";
                        case PCI_PROGIF_IPMI_BLOCK_TRANSFER:
                            return "Serial Bus Controller - IPMI Interface (Block Transfer)";
                        case PCI_PROGIF_IPMI_SERCOS_INTERFACE:
                            return "Serial Bus Controller - IPMI Interface (SERCOS Interface)";
                        case PCI_PROGIF_IPMI_CANBUS_CONTROLLER:
                            return "Serial Bus Controller - IPMI Interface (CANBUS Controller)";
                        default:
                            return "Serial Bus Controller - IPMI Interface (Unknown Prog IF)";
                    }
                case PCI_SUBCLASS_SERIAL_BUS_OTHER:
                    return "Serial Bus Controller - Other Serial Bus Controller";
                default:
                    return "Serial Bus Controller - Unknown Subclass";
            }

        case PCI_CLASS_WIRELESS_CONTROLLER:
            switch (header->subclass) {
                case PCI_SUBCLASS_WIRELESS_IRDA:
                    return "Wireless Controller - iRDA Compatible Controller";
                case PCI_SUBCLASS_WIRELESS_CONSUMER_IR:
                    return "Wireless Controller - Consumer IR Controller";
                case PCI_SUBCLASS_WIRELESS_RF:
                    return "Wireless Controller - RF Controller";
                case PCI_SUBCLASS_WIRELESS_BLUETOOTH:
                    return "Wireless Controller - Bluetooth Controller";
                case PCI_SUBCLASS_WIRELESS_BROADBAND:
                    return "Wireless Controller - Broadband Controller";
                case PCI_SUBCLASS_WIRELESS_ETHERNET_802_1A:
                    return "Wireless Controller - Ethernet Controller (802.1a)";
                case PCI_SUBCLASS_WIRELESS_ETHERNET_802_1B:
                    return "Wireless Controller - Ethernet Controller (802.1b)";
                case PCI_SUBCLASS_WIRELESS_OTHER:
                    return "Wireless Controller - Other Wireless Controller";
                default:
                    return "Wireless Controller - Unknown Subclass";
            }

        case PCI_CLASS_INTELLIGENT_CONTROLLER:
            switch (header->subclass) {
                case PCI_SUBCLASS_INTELLIGENT_I20:
                    return "Intelligent Controller - I20";
                case PCI_SUBCLASS_INTELLIGENT_SATELLITE_COMM:
                    switch (header->prog_if) {
                        case PCI_PROGIF_SATELLITE_TV_CONTROLLER:
                            return "Intelligent Controller - Satellite Communication Controller (TV Controller)";
                        case PCI_PROGIF_SATELLITE_AUDIO_CONTROLLER:
                            return "Intelligent Controller - Satellite Communication Controller (Audio Controller)";
                        case PCI_PROGIF_SATELLITE_VOICE_CONTROLLER:
                            return "Intelligent Controller - Satellite Communication Controller (Voice Controller)";
                        case PCI_PROGIF_SATELLITE_DATA_CONTROLLER:
                            return "Intelligent Controller - Satellite Communication Controller (Data Controller)";
                        default:
                            return "Intelligent Controller - Satellite Communication Controller (Unknown Prog IF)";
                    }
                case PCI_SUBCLASS_INTELLIGENT_ENCRYPTION:
                    switch (header->prog_if) {
                        case PCI_PROGIF_ENCRYPTION_NETWORK_COMPUTING:
                            return "Intelligent Controller - Encryption Controller (Network Computing)";
                        case PCI_PROGIF_ENCRYPTION_ENTERTAINMENT:
                            return "Intelligent Controller - Encryption Controller (Entertainment)";
                        case PCI_PROGIF_ENCRYPTION_OTHER:
                            return "Intelligent Controller - Encryption Controller (Other)";
                        default:
                            return "Intelligent Controller - Encryption Controller (Unknown Prog IF)";
                    }
                case PCI_SUBCLASS_INTELLIGENT_SIGNAL_PROCESSING:
                    switch (header->prog_if) {
                        case PCI_PROGIF_SIGNAL_DPIO_MODULES:
                            return "Intelligent Controller - Signal Processing Controller (DPIO Modules)";
                        case PCI_PROGIF_SIGNAL_PERFORMANCE_COUNTERS:
                            return "Intelligent Controller - Signal Processing Controller (Performance Counters)";
                        case PCI_PROGIF_SIGNAL_COMMUNICATION_SYNCHRONIZER:
                            return "Intelligent Controller - Signal Processing Controller (Communication Synchronizer)";
                        case PCI_PROGIF_SIGNAL_PROCESSING_MANAGEMENT:
                            return "Intelligent Controller - Signal Processing Controller (Processing Management)";
                        case PCI_PROGIF_SIGNAL_OTHER:
                            return "Intelligent Controller - Signal Processing Controller (Other)";
                        default:
                            return "Intelligent Controller - Signal Processing Controller (Unknown Prog IF)";
                    }
                case PCI_SUBCLASS_INTELLIGENT_PROCESSING_ACCELERATOR:
                    return "Intelligent Controller - Processing Accelerator";
                case PCI_SUBCLASS_INTELLIGENT_NON_ESSENTIAL_INSTRUMENTATION:
                    return "Intelligent Controller - Non-Essential Instrumentation";
                case PCI_SUBCLASS_INTELLIGENT_RESERVED:
                    return "Intelligent Controller - Reserved";
                case PCI_SUBCLASS_INTELLIGENT_CO_PROCESSOR:
                    return "Intelligent Controller - Co-Processor";
                case PCI_SUBCLASS_INTELLIGENT_RESERVED_41:
                    return "Intelligent Controller - Reserved";
                case PCI_SUBCLASS_INTELLIGENT_UNASSIGNED_VENDOR_SPECIFIC:
                    return "Intelligent Controller - Unassigned Class (Vendor Specific)";
                default:
                    return "Intelligent Controller - Unknown Subclass";
            }

        case PCI_CLASS_UNASSIGNED_VENDOR_SPECIFIC:
            return "Unassigned Class (Vendor Specific)";

        default:
            return "Unknown PCI Class";
    }
}

uint64_t getBarFromPciHeader(pci_device_header* header, size_t barIndex) {
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

void print_pci_device_info(pci_device_header* header) {
    serial::com1_printf("   PCI Device %04x:%04x - %s\n", header->vendor_id, header->device_id, get_pci_device_name(header));
    render_string("   Found PCI Device: ");
    render_string(get_pci_device_name(header));
    render_string("\n");

    for (int i = 0; i < 6; i++) {
        uint64_t bar = getBarFromPciHeader(header, i);
        if (bar != 0) {
            serial::com1_printf("      BAR[%d]: 0x%08llx\n", i, bar);

            if (header->class_code == PCI_CLASS_SIMPLE_COMMUNICATION_CONTROLLER &&
                header->subclass == PCI_SUBCLASS_SIMPLE_COMM_SERIAL) {
                char bar_str[128] = { 0 };
                sprintf(bar_str, 128, "      BAR[%d]: 0x%08llx\n", i, bar);

                render_string(bar_str);
            }
        }
    }
}

void enumeratePciFunction(uint64_t deviceAddress, uint64_t function) {
    uint64_t function_offset = function << 12;

    uint64_t functionAddress = deviceAddress + function_offset;
    void* functionVirtualAddress = vmm::map_physical_page(functionAddress, DEFAULT_MAPPING_FLAGS);

    pci_device_header* pciDeviceHeader = (pci_device_header*)functionVirtualAddress;

    if (pciDeviceHeader->device_id == 0 || pciDeviceHeader->device_id == 0xffff) {
        vmm::unmap_virtual_page((uintptr_t)functionVirtualAddress);
        return;
    }

    print_pci_device_info(pciDeviceHeader);

    if (pciDeviceHeader->class_code == PCI_CLASS_SIMPLE_COMMUNICATION_CONTROLLER &&
        pciDeviceHeader->subclass == PCI_SUBCLASS_SIMPLE_COMM_SERIAL &&
        pciDeviceHeader->prog_if == PCI_PROGIF_SERIAL_16550_COMPATIBLE    
    ) {
        render_string("FOUND PCI SERIAL CONTROLLER\n");
        uint64_t bus = (deviceAddress >> 20) & 0xFF;
        uint64_t device = (deviceAddress >> 15) & 0x1F;
        uint32_t bar0 = pciDeviceHeader->bar[0];
        uint32_t bar5 = pciDeviceHeader->bar[5];

        char buf[128] = { 0 };
        sprintf(buf, 127, "bar0_literal: 0x%x\n", bar0);
        render_string(buf);

        sprintf(buf, 127, "bar5_literal: 0x%x\n", bar5);
        render_string(buf);

        // Decode BAR0 as I/O base
        if (bar0 & 0x1) { // I/O space
            uint16_t io_base = bar0 & ~0x3;
            serial::com1_printf("Decoded I/O base: 0x%x\n", io_base);

            sprintf(buf, 127, "Decoded I/O base: 0x%x\n", io_base);
            render_string(buf);

            // Enable I/O space in PCI command register
            uint16_t command = pci_read_config_word(bus, device, function, 0x04);
            pci_write_config_word(bus, device, function, 0x04, command | 0x1);

            //uart_initialize(io_base);
            // serial::init_port(io_base);
            // serial::write(io_base, "This is a serial UART message!\n");

            init_port(io_base);
            write_string(io_base, "UART messages work!\n");
            write_string(io_base, "Welcome to Stellux!\n");
        } else {
            render_string("BAR0 is not I/O space\n");
        }
    }
}

void enumeratePciDevice(uint64_t busAddress, uint64_t device) {
    uint64_t device_offset = device << 15;
    uint64_t deviceAddress = busAddress + device_offset;

    void* deviceVirtualAddress = vmm::map_physical_page(deviceAddress, DEFAULT_MAPPING_FLAGS);

    pci_device_header* pciDeviceHeader = (pci_device_header*)deviceVirtualAddress;

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

    pci_device_header* pciDeviceHeader = (pci_device_header*)busVirtualAddress;

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
