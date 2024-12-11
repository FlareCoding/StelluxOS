#include <pci/pci_class_codes.h>
#include <pci/pci.h>

namespace pci {
const char* get_pci_device_name(const pci_function_desc* header) {
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
} // namespace pci
