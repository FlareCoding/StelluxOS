#ifndef PCI_CLASS_CODES_H
#define PCI_CLASS_CODES_H

// PCI Class Codes, Subclasses, and Programming Interfaces

// Class Code 0x00 - Unclassified
#define PCI_CLASS_UNCLASSIFIED                0x00

    // Subclass 0x00 - Non-VGA-Compatible Unclassified Device
    #define PCI_SUBCLASS_UNCLASSIFIED_NON_VGA_COMPATIBLE 0x00

// Class Code 0x01 - Mass Storage Controller
#define PCI_CLASS_MASS_STORAGE_CONTROLLER    0x01

    // Subclass 0x00 - SCSI Bus Controller
    #define PCI_SUBCLASS_MASS_STORAGE_SCSI       0x00

    // Subclass 0x01 - IDE Controller
    #define PCI_SUBCLASS_MASS_STORAGE_IDE        0x01

        // Prog IF for IDE Controller
        #define PCI_PROGIF_IDE_ISA_COMPATIBILITY_MODE_ONLY        0x00
        #define PCI_PROGIF_IDE_PCI_NATIVE_MODE_ONLY                0x05
        #define PCI_PROGIF_IDE_ISA_PCI_NATIVE_SUPPORT_BUS_MASTERING 0x8A
        #define PCI_PROGIF_IDE_PCI_ISA_NATIVE_SUPPORT_BUS_MASTERING 0x8F

    // Subclass 0x02 - Floppy Disk Controller
    #define PCI_SUBCLASS_MASS_STORAGE_FLOPPY     0x02

    // Subclass 0x03 - IPI Bus Controller
    #define PCI_SUBCLASS_MASS_STORAGE_IPI         0x03

    // Subclass 0x04 - RAID Controller
    #define PCI_SUBCLASS_MASS_STORAGE_RAID        0x04

    // Subclass 0x05 - ATA Controller
    #define PCI_SUBCLASS_MASS_STORAGE_ATA         0x05

        // Prog IF for ATA Controller
        #define PCI_PROGIF_ATA_SINGLE_DMA            0x20
        #define PCI_PROGIF_ATA_CHAINED_DMA           0x30

    // Subclass 0x06 - Serial ATA Controller
    #define PCI_SUBCLASS_MASS_STORAGE_SATA        0x06

        // Prog IF for Serial ATA Controller
        #define PCI_PROGIF_SATA_VENDOR_SPECIFIC      0x00
        #define PCI_PROGIF_SATA_AHCI_1_0             0x01
        #define PCI_PROGIF_SATA_SERIAL_STORAGE_BUS   0x02

    // Subclass 0x07 - Serial Attached SCSI Controller
    #define PCI_SUBCLASS_MASS_STORAGE_SAS         0x07

        // Prog IF for Serial Attached SCSI Controller
        #define PCI_PROGIF_SAS_SAS                   0x00
        #define PCI_PROGIF_SAS_SERIAL_STORAGE_BUS    0x01

    // Subclass 0x08 - Non-Volatile Memory Controller
    #define PCI_SUBCLASS_MASS_STORAGE_NVME        0x08

        // Prog IF for Non-Volatile Memory Controller
        #define PCI_PROGIF_NVME_NVMHCI                0x01
        #define PCI_PROGIF_NVME_NVM_EXPRESS           0x02

    // Subclass 0x80 - Other Mass Storage Controller
    #define PCI_SUBCLASS_MASS_STORAGE_OTHER       0x80

// Class Code 0x02 - Network Controller
#define PCI_CLASS_NETWORK_CONTROLLER         0x02

    // Subclass 0x00 - Ethernet Controller
    #define PCI_SUBCLASS_NETWORK_ETHERNET         0x00

    // Subclass 0x01 - Token Ring Controller
    #define PCI_SUBCLASS_NETWORK_TOKEN_RING       0x01

    // Subclass 0x02 - FDDI Controller
    #define PCI_SUBCLASS_NETWORK_FDDI              0x02

    // Subclass 0x03 - ATM Controller
    #define PCI_SUBCLASS_NETWORK_ATM               0x03

    // Subclass 0x04 - ISDN Controller
    #define PCI_SUBCLASS_NETWORK_ISDN              0x04

    // Subclass 0x05 - WorldFip Controller
    #define PCI_SUBCLASS_NETWORK_WORLDFIP          0x05

    // Subclass 0x06 - PICMG 2.14 Multi Computing Controller
    #define PCI_SUBCLASS_NETWORK_PICMG_2_14        0x06

    // Subclass 0x07 - Infiniband Controller
    #define PCI_SUBCLASS_NETWORK_INFINIBAND        0x07

    // Subclass 0x08 - Fabric Controller
    #define PCI_SUBCLASS_NETWORK_FABRIC            0x08

    // Subclass 0x80 - Other Network Controller
    #define PCI_SUBCLASS_NETWORK_OTHER             0x80

// Class Code 0x03 - Display Controller
#define PCI_CLASS_DISPLAY_CONTROLLER         0x03

    // Subclass 0x00 - VGA Compatible Controller
    #define PCI_SUBCLASS_DISPLAY_VGA_COMPATIBLE    0x00

        // Prog IF for VGA Compatible Controller
        #define PCI_PROGIF_VGA_CONTROLLER              0x00
        #define PCI_PROGIF_8514_COMPATIBLE_CONTROLLER  0x01
        #define PCI_PROGIF_XGA_CONTROLLER               0x01
        #define PCI_PROGIF_3D_CONTROLLER                0x02

    // Subclass 0x80 - Other Display Controller
    #define PCI_SUBCLASS_DISPLAY_OTHER             0x80

// Class Code 0x04 - Multimedia Controller
#define PCI_CLASS_MULTIMEDIA_CONTROLLER      0x04

    // Subclass 0x00 - Multimedia Video Controller
    #define PCI_SUBCLASS_MULTIMEDIA_VIDEO           0x00

    // Subclass 0x01 - Multimedia Audio Controller
    #define PCI_SUBCLASS_MULTIMEDIA_AUDIO           0x01

    // Subclass 0x02 - Computer Telephony Device
    #define PCI_SUBCLASS_MULTIMEDIA_TELEPHONY       0x02

    // Subclass 0x03 - Audio Device
    #define PCI_SUBCLASS_MULTIMEDIA_AUDIO_DEVICE    0x03

    // Subclass 0x80 - Other Multimedia Controller
    #define PCI_SUBCLASS_MULTIMEDIA_OTHER           0x80

// Class Code 0x05 - Memory Controller
#define PCI_CLASS_MEMORY_CONTROLLER          0x05

    // Subclass 0x00 - RAM Controller
    #define PCI_SUBCLASS_MEMORY_RAM                 0x00

    // Subclass 0x01 - Flash Controller
    #define PCI_SUBCLASS_MEMORY_FLASH               0x01

    // Subclass 0x80 - Other Memory Controller
    #define PCI_SUBCLASS_MEMORY_OTHER               0x80

// Class Code 0x06 - Bridge
#define PCI_CLASS_BRIDGE                      0x06

    // Subclass 0x00 - Host Bridge
    #define PCI_SUBCLASS_BRIDGE_HOST                0x00

    // Subclass 0x01 - ISA Bridge
    #define PCI_SUBCLASS_BRIDGE_ISA                 0x01

    // Subclass 0x02 - EISA Bridge
    #define PCI_SUBCLASS_BRIDGE_EISA                0x02

    // Subclass 0x03 - MCA Bridge
    #define PCI_SUBCLASS_BRIDGE_MCA                 0x03

    // Subclass 0x04 - PCI-to-PCI Bridge
    #define PCI_SUBCLASS_BRIDGE_PCI_TO_PCI          0x04

        // Prog IF for PCI-to-PCI Bridge
        #define PCI_PROGIF_PCI_TO_PCI_NORMAL_DECODE          0x00
        #define PCI_PROGIF_PCI_TO_PCI_SUBTRACTIVE_DECODE     0x01

    // Subclass 0x05 - PCMCIA Bridge
    #define PCI_SUBCLASS_BRIDGE_PCMCIA               0x05

    // Subclass 0x06 - NuBus Bridge
    #define PCI_SUBCLASS_BRIDGE_NUBUS                0x06

    // Subclass 0x07 - CardBus Bridge
    #define PCI_SUBCLASS_BRIDGE_CARDBUS              0x07

    // Subclass 0x08 - RACEway Bridge
    #define PCI_SUBCLASS_BRIDGE_RACEWAY              0x08

        // Prog IF for RACEway Bridge
        #define PCI_PROGIF_RACEWAY_TRANSPARENT_MODE          0x00
        #define PCI_PROGIF_RACEWAY_ENDPOINT_MODE             0x01

    // Subclass 0x09 - PCI-to-PCI Bridge (Additional)
    #define PCI_SUBCLASS_BRIDGE_PCI_TO_PCI_ADDITIONAL   0x09

        // Prog IF for Additional PCI-to-PCI Bridge
        #define PCI_PROGIF_PCI_TO_PCI_SEMI_TRANSPARENT_PRIMARY 0x40
        #define PCI_PROGIF_PCI_TO_PCI_SEMI_TRANSPARENT_SECONDARY 0x80

    // Subclass 0x0A - InfiniBand-to-PCI Host Bridge
    #define PCI_SUBCLASS_BRIDGE_INFINIBAND_TO_PCI     0x0A

    // Subclass 0x80 - Other Bridge
    #define PCI_SUBCLASS_BRIDGE_OTHER                 0x80

// Class Code 0x07 - Simple Communication Controller
#define PCI_CLASS_SIMPLE_COMMUNICATION_CONTROLLER 0x07

    // Subclass 0x00 - Serial Controller
    #define PCI_SUBCLASS_SIMPLE_COMM_SERIAL            0x00

        // Prog IF for Serial Controller
        #define PCI_PROGIF_SERIAL_8250_COMPATIBLE         0x00
        #define PCI_PROGIF_SERIAL_16450_COMPATIBLE        0x01
        #define PCI_PROGIF_SERIAL_16550_COMPATIBLE        0x02
        #define PCI_PROGIF_SERIAL_16650_COMPATIBLE        0x03
        #define PCI_PROGIF_SERIAL_16750_COMPATIBLE        0x04
        #define PCI_PROGIF_SERIAL_16850_COMPATIBLE        0x05
        #define PCI_PROGIF_SERIAL_16950_COMPATIBLE        0x06

    // Subclass 0x01 - Parallel Controller
    #define PCI_SUBCLASS_SIMPLE_COMM_PARALLEL          0x01

        // Prog IF for Parallel Controller
        #define PCI_PROGIF_PARALLEL_STANDARD_PORT         0x00
        #define PCI_PROGIF_PARALLEL_BIDIRECTIONAL_PORT    0x01
        #define PCI_PROGIF_PARALLEL_ECP_1_X               0x02
        #define PCI_PROGIF_PARALLEL_IEEE_1284_CONTROLLER   0x03
        #define PCI_PROGIF_PARALLEL_IEEE_1284_TARGET_DEVICE 0xFE

    // Subclass 0x02 - Multiport Serial Controller
    #define PCI_SUBCLASS_SIMPLE_COMM_MULTIPORT_SERIAL  0x02

    // Subclass 0x03 - Modem
    #define PCI_SUBCLASS_SIMPLE_COMM_MODEM              0x03

        // Prog IF for Modem
        #define PCI_PROGIF_MODEM_GENERIC                   0x00
        #define PCI_PROGIF_MODEM_HAYES_16450_COMPATIBLE     0x01
        #define PCI_PROGIF_MODEM_HAYES_16550_COMPATIBLE     0x02
        #define PCI_PROGIF_MODEM_HAYES_16650_COMPATIBLE     0x03
        #define PCI_PROGIF_MODEM_HAYES_16750_COMPATIBLE     0x04

    // Subclass 0x04 - IEEE 488.1/2 (GPIB) Controller
    #define PCI_SUBCLASS_SIMPLE_COMM_GPIP               0x04

    // Subclass 0x05 - Smart Card Controller
    #define PCI_SUBCLASS_SIMPLE_COMM_SMART_CARD         0x05

    // Subclass 0x80 - Other Simple Communication Controller
    #define PCI_SUBCLASS_SIMPLE_COMM_OTHER              0x80

// Class Code 0x08 - Base System Peripheral
#define PCI_CLASS_BASE_SYSTEM_PERIPHERAL      0x08

    // Subclass 0x00 - PIC
    #define PCI_SUBCLASS_BASE_SYSTEM_PIC               0x00

        // Prog IF for PIC
        #define PCI_PROGIF_PIC_GENERIC_8259_COMPATIBLE     0x00
        #define PCI_PROGIF_PIC_ISA_COMPATIBLE               0x01
        #define PCI_PROGIF_PIC_EISA_COMPATIBLE              0x02
        #define PCI_PROGIF_PIC_IO_APIC_INTERRUPT_CONTROLLER 0x10
        #define PCI_PROGIF_PIC_IO_X_APIC_INTERRUPT_CONTROLLER 0x20

    // Subclass 0x01 - DMA Controller
    #define PCI_SUBCLASS_BASE_SYSTEM_DMA                0x01

        // Prog IF for DMA Controller
        #define PCI_PROGIF_DMA_GENERIC_8237_COMPATIBLE     0x00
        #define PCI_PROGIF_DMA_ISA_COMPATIBLE               0x01
        #define PCI_PROGIF_DMA_EISA_COMPATIBLE              0x02

    // Subclass 0x02 - Timer
    #define PCI_SUBCLASS_BASE_SYSTEM_TIMER              0x02

        // Prog IF for Timer
        #define PCI_PROGIF_TIMER_GENERIC_8254_COMPATIBLE   0x00
        #define PCI_PROGIF_TIMER_ISA_COMPATIBLE             0x01
        #define PCI_PROGIF_TIMER_EISA_COMPATIBLE            0x02
        #define PCI_PROGIF_TIMER_HPET                       0x03

    // Subclass 0x03 - RTC Controller
    #define PCI_SUBCLASS_BASE_SYSTEM_RTC                0x03

        // Prog IF for RTC Controller
        #define PCI_PROGIF_RTC_GENERIC                     0x00
        #define PCI_PROGIF_RTC_ISA_COMPATIBLE              0x01

    // Subclass 0x04 - PCI Hot-Plug Controller
    #define PCI_SUBCLASS_BASE_SYSTEM_PCI_HOT_PLUG       0x04

    // Subclass 0x05 - SD Host Controller
    #define PCI_SUBCLASS_BASE_SYSTEM_SD_HOST             0x05

    // Subclass 0x06 - IOMMU
    #define PCI_SUBCLASS_BASE_SYSTEM_IOMMU               0x06

    // Subclass 0x80 - Other Base System Peripheral
    #define PCI_SUBCLASS_BASE_SYSTEM_OTHER               0x80

// Class Code 0x09 - Input Device Controller
#define PCI_CLASS_INPUT_DEVICE_CONTROLLER      0x09

    // Subclass 0x00 - Keyboard Controller
    #define PCI_SUBCLASS_INPUT_KEYBOARD                 0x00

    // Subclass 0x01 - Digitizer Pen
    #define PCI_SUBCLASS_INPUT_DIGITIZER_PEN            0x01

    // Subclass 0x02 - Mouse Controller
    #define PCI_SUBCLASS_INPUT_MOUSE                    0x02

    // Subclass 0x03 - Scanner Controller
    #define PCI_SUBCLASS_INPUT_SCANNER                  0x03

    // Subclass 0x04 - Gameport Controller
    #define PCI_SUBCLASS_INPUT_GAMEPORT                 0x04

        // Prog IF for Gameport Controller
        #define PCI_PROGIF_GAMEPORT_GENERIC                0x00
        #define PCI_PROGIF_GAMEPORT_EXTENDED               0x10

    // Subclass 0x80 - Other Input Device Controller
    #define PCI_SUBCLASS_INPUT_OTHER                    0x80

// Class Code 0x0A - Docking Station
#define PCI_CLASS_DOCKING_STATION              0x0A

    // Subclass 0x00 - Generic Docking Station
    #define PCI_SUBCLASS_DOCKING_GENERIC                0x00

    // Subclass 0x80 - Other Docking Station
    #define PCI_SUBCLASS_DOCKING_OTHER                  0x80

// Class Code 0x0B - Processor
#define PCI_CLASS_PROCESSOR                    0x0B

    // Subclass 0x00 - 386 Processor
    #define PCI_SUBCLASS_PROCESSOR_386                  0x00

    // Subclass 0x01 - 486 Processor
    #define PCI_SUBCLASS_PROCESSOR_486                  0x01

    // Subclass 0x02 - Pentium Processor
    #define PCI_SUBCLASS_PROCESSOR_PENTIUM              0x02

    // Subclass 0x03 - Pentium Pro Processor
    #define PCI_SUBCLASS_PROCESSOR_PENTIUM_PRO          0x03

    // Subclass 0x10 - Alpha Processor
    #define PCI_SUBCLASS_PROCESSOR_ALPHA                 0x10

    // Subclass 0x20 - PowerPC Processor
    #define PCI_SUBCLASS_PROCESSOR_POWERPC               0x20

    // Subclass 0x30 - MIPS Processor
    #define PCI_SUBCLASS_PROCESSOR_MIPS                  0x30

    // Subclass 0x40 - Co-Processor
    #define PCI_SUBCLASS_PROCESSOR_CO_PROCESSOR          0x40

    // Subclass 0x80 - Other Processor
    #define PCI_SUBCLASS_PROCESSOR_OTHER                 0x80

// Class Code 0x0C - Serial Bus Controller
#define PCI_CLASS_SERIAL_BUS_CONTROLLER        0x0C

    // Subclass 0x00 - FireWire (IEEE 1394) Controller
    #define PCI_SUBCLASS_SERIAL_BUS_FIREWIRE            0x00

        // Prog IF for FireWire Controller
        #define PCI_PROGIF_FIREWIRE_GENERIC                0x00
        #define PCI_PROGIF_FIREWIRE_OHCI                   0x10
        #define PCI_PROGIF_FIREWIRE_ACCESS_BUS_CONTROLLER  0x01
        #define PCI_PROGIF_FIREWIRE_SSA                    0x02

    // Subclass 0x03 - USB Controller
    #define PCI_SUBCLASS_SERIAL_BUS_USB                  0x03

        // Prog IF for USB Controller
        #define PCI_PROGIF_USB_UHCI                         0x00
        #define PCI_PROGIF_USB_OHCI                         0x10
        #define PCI_PROGIF_USB_EHCI                         0x20
        #define PCI_PROGIF_USB_XHCI                         0x30
        #define PCI_PROGIF_USB_UNSPECIFIED                  0x80
        #define PCI_PROGIF_USB_DEVICE_NOT_HOST_CONTROLLER   0xFE

    // Subclass 0x04 - Fibre Channel
    #define PCI_SUBCLASS_SERIAL_BUS_FIBRE_CHANNEL         0x04

    // Subclass 0x05 - SMBus Controller
    #define PCI_SUBCLASS_SERIAL_BUS_SMBUS                 0x05

    // Subclass 0x06 - InfiniBand Controller
    #define PCI_SUBCLASS_SERIAL_BUS_INFINIBAND            0x06

    // Subclass 0x07 - IPMI Interface
    #define PCI_SUBCLASS_SERIAL_BUS_IPMI                   0x07

        // Prog IF for IPMI Interface
        #define PCI_PROGIF_IPMI_SMIC                         0x00
        #define PCI_PROGIF_IPMI_KEYBOARD_CONTROLLER_STYLE    0x01
        #define PCI_PROGIF_IPMI_BLOCK_TRANSFER               0x02
        #define PCI_PROGIF_IPMI_SERCOS_INTERFACE             0x08
        #define PCI_PROGIF_IPMI_CANBUS_CONTROLLER            0x09

    // Subclass 0x80 - Other Serial Bus Controller
    #define PCI_SUBCLASS_SERIAL_BUS_OTHER                  0x80

// Class Code 0x0D - Wireless Controller
#define PCI_CLASS_WIRELESS_CONTROLLER           0x0D

    // Subclass 0x00 - iRDA Compatible Controller
    #define PCI_SUBCLASS_WIRELESS_IRDA                    0x00

    // Subclass 0x01 - Consumer IR Controller
    #define PCI_SUBCLASS_WIRELESS_CONSUMER_IR             0x01

    // Subclass 0x10 - RF Controller
    #define PCI_SUBCLASS_WIRELESS_RF                      0x10

    // Subclass 0x11 - Bluetooth Controller
    #define PCI_SUBCLASS_WIRELESS_BLUETOOTH               0x11

    // Subclass 0x12 - Broadband Controller
    #define PCI_SUBCLASS_WIRELESS_BROADBAND               0x12

    // Subclass 0x20 - Ethernet Controller (802.1a)
    #define PCI_SUBCLASS_WIRELESS_ETHERNET_802_1A         0x20

    // Subclass 0x21 - Ethernet Controller (802.1b)
    #define PCI_SUBCLASS_WIRELESS_ETHERNET_802_1B         0x21

    // Subclass 0x80 - Other Wireless Controller
    #define PCI_SUBCLASS_WIRELESS_OTHER                   0x80

// Class Code 0x0E - Intelligent Controller
#define PCI_CLASS_INTELLIGENT_CONTROLLER        0x0E

    // Subclass 0x00 - I20
    #define PCI_SUBCLASS_INTELLIGENT_I20                  0x00

    // Subclass 0x0F - Satellite Communication Controller
    #define PCI_SUBCLASS_INTELLIGENT_SATELLITE_COMM        0x0F

        // Prog IF for Satellite Communication Controller
        #define PCI_PROGIF_SATELLITE_TV_CONTROLLER            0x01
        #define PCI_PROGIF_SATELLITE_AUDIO_CONTROLLER         0x02
        #define PCI_PROGIF_SATELLITE_VOICE_CONTROLLER         0x03
        #define PCI_PROGIF_SATELLITE_DATA_CONTROLLER          0x04

    // Subclass 0x10 - Encryption Controller
    #define PCI_SUBCLASS_INTELLIGENT_ENCRYPTION            0x10

        // Prog IF for Encryption Controller
        #define PCI_PROGIF_ENCRYPTION_NETWORK_COMPUTING       0x00
        #define PCI_PROGIF_ENCRYPTION_ENTERTAINMENT            0x10
        #define PCI_PROGIF_ENCRYPTION_OTHER                    0x80

    // Subclass 0x11 - Signal Processing Controller
    #define PCI_SUBCLASS_INTELLIGENT_SIGNAL_PROCESSING      0x11

        // Prog IF for Signal Processing Controller
        #define PCI_PROGIF_SIGNAL_DPIO_MODULES                0x00
        #define PCI_PROGIF_SIGNAL_PERFORMANCE_COUNTERS         0x01
        #define PCI_PROGIF_SIGNAL_COMMUNICATION_SYNCHRONIZER   0x10
        #define PCI_PROGIF_SIGNAL_PROCESSING_MANAGEMENT        0x20
        #define PCI_PROGIF_SIGNAL_OTHER                        0x80

    // Subclass 0x12 - Processing Accelerator
    #define PCI_SUBCLASS_INTELLIGENT_PROCESSING_ACCELERATOR 0x12

    // Subclass 0x13 - Non-Essential Instrumentation
    #define PCI_SUBCLASS_INTELLIGENT_NON_ESSENTIAL_INSTRUMENTATION 0x13

    // Subclass 0x14 - Reserved
    #define PCI_SUBCLASS_INTELLIGENT_RESERVED              0x14

    // Subclass 0x40 - Co-Processor
    #define PCI_SUBCLASS_INTELLIGENT_CO_PROCESSOR          0x40

    // Subclass 0x41 - Reserved
    #define PCI_SUBCLASS_INTELLIGENT_RESERVED_41           0x41

    // Subclass 0xFF - Unassigned Class (Vendor Specific)
    #define PCI_SUBCLASS_INTELLIGENT_UNASSIGNED_VENDOR_SPECIFIC 0xFF

// Class Code 0xFF - Unassigned Class (Vendor Specific)
#define PCI_CLASS_UNASSIGNED_VENDOR_SPECIFIC     0xFF

namespace pci {
struct pci_function_desc;
const char* get_pci_device_name(const pci_function_desc* header);
} // namespace pci

#endif // PCI_CLASS_CODES_H
