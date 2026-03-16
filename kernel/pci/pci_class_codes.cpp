#include "pci/pci_class_codes.h"

namespace pci {

const char* device_class_name(uint8_t cls, uint8_t sub, uint8_t prog) {
    switch (cls) {
        case CLASS_UNCLASSIFIED:
            return "Unclassified device";

        case CLASS_MASS_STORAGE:
            switch (sub) {
                case SUB_STORAGE_SCSI:   return "SCSI controller";
                case SUB_STORAGE_IDE:    return "IDE controller";
                case SUB_STORAGE_FLOPPY: return "Floppy controller";
                case SUB_STORAGE_IPI:    return "IPI bus controller";
                case SUB_STORAGE_RAID:   return "RAID controller";
                case SUB_STORAGE_ATA:    return "ATA controller";
                case SUB_STORAGE_SATA:
                    switch (prog) {
                        case PROGIF_SATA_VENDOR: return "SATA controller (vendor)";
                        case PROGIF_SATA_AHCI:   return "SATA controller (AHCI)";
                        default:                 return "SATA controller";
                    }
                case SUB_STORAGE_SAS:    return "SAS controller";
                case SUB_STORAGE_NVME:
                    switch (prog) {
                        case PROGIF_NVME_NVMHCI:  return "NVMe controller (NVMHCI)";
                        case PROGIF_NVME_EXPRESS:  return "NVMe controller (NVM Express)";
                        default:                  return "NVMe controller";
                    }
                default: return "Mass storage controller";
            }

        case CLASS_NETWORK:
            switch (sub) {
                case SUB_NET_ETHERNET:    return "Ethernet controller";
                case SUB_NET_TOKEN_RING:  return "Token Ring controller";
                case SUB_NET_FDDI:        return "FDDI controller";
                case SUB_NET_ATM:         return "ATM controller";
                case SUB_NET_ISDN:        return "ISDN controller";
                case SUB_NET_INFINIBAND:  return "InfiniBand controller";
                case SUB_NET_FABRIC:      return "Fabric controller";
                default:                  return "Network controller";
            }

        case CLASS_DISPLAY:
            switch (sub) {
                case SUB_DISPLAY_VGA:
                    switch (prog) {
                        case PROGIF_VGA: return "VGA compatible controller";
                        case PROGIF_3D:  return "3D controller";
                        default:         return "VGA controller";
                    }
                default: return "Display controller";
            }

        case CLASS_MULTIMEDIA:
            switch (sub) {
                case SUB_MM_VIDEO:        return "Multimedia video controller";
                case SUB_MM_AUDIO:        return "Audio controller";
                case SUB_MM_TELEPHONY:    return "Telephony device";
                case SUB_MM_AUDIO_DEVICE: return "HD Audio device";
                default:                  return "Multimedia controller";
            }

        case CLASS_MEMORY:
            switch (sub) {
                case SUB_MEM_RAM:   return "RAM controller";
                case SUB_MEM_FLASH: return "Flash controller";
                default:            return "Memory controller";
            }

        case CLASS_BRIDGE:
            switch (sub) {
                case SUB_BRIDGE_HOST:    return "Host bridge";
                case SUB_BRIDGE_ISA:     return "ISA bridge";
                case SUB_BRIDGE_EISA:    return "EISA bridge";
                case SUB_BRIDGE_MCA:     return "MCA bridge";
                case SUB_BRIDGE_PCI:
                    switch (prog) {
                        case PROGIF_PCI_NORMAL:      return "PCI-to-PCI bridge";
                        case PROGIF_PCI_SUBTRACTIVE: return "PCI-to-PCI bridge (subtractive)";
                        default:                     return "PCI-to-PCI bridge";
                    }
                case SUB_BRIDGE_PCMCIA:  return "PCMCIA bridge";
                case SUB_BRIDGE_NUBUS:   return "NuBus bridge";
                case SUB_BRIDGE_CARDBUS: return "CardBus bridge";
                case SUB_BRIDGE_RACEWAY: return "RACEway bridge";
                default:                 return "Bridge device";
            }

        case CLASS_SIMPLE_COMM:
            switch (sub) {
                case SUB_COMM_SERIAL:    return "Serial controller";
                case SUB_COMM_PARALLEL:  return "Parallel controller";
                case SUB_COMM_MULTIPORT: return "Multiport serial controller";
                case SUB_COMM_MODEM:     return "Modem";
                default:                 return "Communication controller";
            }

        case CLASS_BASE_SYSTEM:
            switch (sub) {
                case SUB_BASE_PIC:     return "PIC";
                case SUB_BASE_DMA:     return "DMA controller";
                case SUB_BASE_TIMER:   return "Timer";
                case SUB_BASE_RTC:     return "RTC controller";
                case SUB_BASE_HOTPLUG: return "PCI Hot-Plug controller";
                case SUB_BASE_SD:      return "SD Host controller";
                case SUB_BASE_IOMMU:   return "IOMMU";
                default:               return "System peripheral";
            }

        case CLASS_INPUT:
            switch (sub) {
                case SUB_INPUT_KEYBOARD: return "Keyboard controller";
                case SUB_INPUT_PEN:      return "Digitizer pen";
                case SUB_INPUT_MOUSE:    return "Mouse controller";
                case SUB_INPUT_SCANNER:  return "Scanner controller";
                case SUB_INPUT_GAMEPORT: return "Gameport controller";
                default:                 return "Input device controller";
            }

        case CLASS_DOCKING:
            return "Docking station";

        case CLASS_PROCESSOR:
            return "Processor";

        case CLASS_SERIAL_BUS:
            switch (sub) {
                case SUB_SERIAL_FIREWIRE:   return "FireWire controller";
                case SUB_SERIAL_USB:
                    switch (prog) {
                        case PROGIF_USB_UHCI:        return "USB controller (UHCI)";
                        case PROGIF_USB_OHCI:        return "USB controller (OHCI)";
                        case PROGIF_USB_EHCI:        return "USB controller (EHCI)";
                        case PROGIF_USB_XHCI:        return "USB controller (XHCI)";
                        case PROGIF_USB_UNSPECIFIED:  return "USB controller";
                        case PROGIF_USB_DEVICE:       return "USB device (not host)";
                        default:                     return "USB controller";
                    }
                case SUB_SERIAL_FIBRE:      return "Fibre Channel";
                case SUB_SERIAL_SMBUS:      return "SMBus controller";
                case SUB_SERIAL_INFINIBAND: return "InfiniBand controller";
                case SUB_SERIAL_IPMI:       return "IPMI interface";
                default:                    return "Serial bus controller";
            }

        case CLASS_WIRELESS:
            switch (sub) {
                case SUB_WIRELESS_IRDA:      return "iRDA controller";
                case SUB_WIRELESS_IR:        return "Consumer IR controller";
                case SUB_WIRELESS_RF:        return "RF controller";
                case SUB_WIRELESS_BLUETOOTH: return "Bluetooth controller";
                case SUB_WIRELESS_BROADBAND: return "Broadband controller";
                default:                     return "Wireless controller";
            }

        case CLASS_INTELLIGENT:
            return "Intelligent controller";

        case CLASS_VENDOR_SPECIFIC:
            return "Vendor specific";

        default:
            return "Unknown device";
    }
}

} // namespace pci
