#ifndef STELLUX_PCI_PCI_CLASS_CODES_H
#define STELLUX_PCI_PCI_CLASS_CODES_H

#include "common/types.h"

namespace pci {

// Class codes
constexpr uint8_t CLASS_UNCLASSIFIED          = 0x00;
constexpr uint8_t CLASS_MASS_STORAGE          = 0x01;
constexpr uint8_t CLASS_NETWORK               = 0x02;
constexpr uint8_t CLASS_DISPLAY               = 0x03;
constexpr uint8_t CLASS_MULTIMEDIA            = 0x04;
constexpr uint8_t CLASS_MEMORY                = 0x05;
constexpr uint8_t CLASS_BRIDGE                = 0x06;
constexpr uint8_t CLASS_SIMPLE_COMM           = 0x07;
constexpr uint8_t CLASS_BASE_SYSTEM           = 0x08;
constexpr uint8_t CLASS_INPUT                 = 0x09;
constexpr uint8_t CLASS_DOCKING               = 0x0A;
constexpr uint8_t CLASS_PROCESSOR             = 0x0B;
constexpr uint8_t CLASS_SERIAL_BUS            = 0x0C;
constexpr uint8_t CLASS_WIRELESS              = 0x0D;
constexpr uint8_t CLASS_INTELLIGENT           = 0x0E;
constexpr uint8_t CLASS_VENDOR_SPECIFIC       = 0xFF;

// Mass Storage subclasses
constexpr uint8_t SUB_STORAGE_SCSI           = 0x00;
constexpr uint8_t SUB_STORAGE_IDE            = 0x01;
constexpr uint8_t SUB_STORAGE_FLOPPY         = 0x02;
constexpr uint8_t SUB_STORAGE_IPI            = 0x03;
constexpr uint8_t SUB_STORAGE_RAID           = 0x04;
constexpr uint8_t SUB_STORAGE_ATA            = 0x05;
constexpr uint8_t SUB_STORAGE_SATA           = 0x06;
constexpr uint8_t SUB_STORAGE_SAS            = 0x07;
constexpr uint8_t SUB_STORAGE_NVME           = 0x08;

// SATA prog IFs
constexpr uint8_t PROGIF_SATA_VENDOR         = 0x00;
constexpr uint8_t PROGIF_SATA_AHCI           = 0x01;

// NVMe prog IFs
constexpr uint8_t PROGIF_NVME_NVMHCI         = 0x01;
constexpr uint8_t PROGIF_NVME_EXPRESS         = 0x02;

// Network subclasses
constexpr uint8_t SUB_NET_ETHERNET           = 0x00;
constexpr uint8_t SUB_NET_TOKEN_RING         = 0x01;
constexpr uint8_t SUB_NET_FDDI              = 0x02;
constexpr uint8_t SUB_NET_ATM               = 0x03;
constexpr uint8_t SUB_NET_ISDN              = 0x04;
constexpr uint8_t SUB_NET_INFINIBAND        = 0x07;
constexpr uint8_t SUB_NET_FABRIC            = 0x08;

// Display subclasses
constexpr uint8_t SUB_DISPLAY_VGA           = 0x00;

// Display prog IFs
constexpr uint8_t PROGIF_VGA                = 0x00;
constexpr uint8_t PROGIF_3D                 = 0x02;

// Multimedia subclasses
constexpr uint8_t SUB_MM_VIDEO              = 0x00;
constexpr uint8_t SUB_MM_AUDIO              = 0x01;
constexpr uint8_t SUB_MM_TELEPHONY          = 0x02;
constexpr uint8_t SUB_MM_AUDIO_DEVICE       = 0x03;

// Memory subclasses
constexpr uint8_t SUB_MEM_RAM               = 0x00;
constexpr uint8_t SUB_MEM_FLASH             = 0x01;

// Bridge subclasses
constexpr uint8_t SUB_BRIDGE_HOST           = 0x00;
constexpr uint8_t SUB_BRIDGE_ISA            = 0x01;
constexpr uint8_t SUB_BRIDGE_EISA           = 0x02;
constexpr uint8_t SUB_BRIDGE_MCA            = 0x03;
constexpr uint8_t SUB_BRIDGE_PCI            = 0x04;
constexpr uint8_t SUB_BRIDGE_PCMCIA         = 0x05;
constexpr uint8_t SUB_BRIDGE_NUBUS          = 0x06;
constexpr uint8_t SUB_BRIDGE_CARDBUS        = 0x07;
constexpr uint8_t SUB_BRIDGE_RACEWAY        = 0x08;

// Bridge prog IFs
constexpr uint8_t PROGIF_PCI_NORMAL         = 0x00;
constexpr uint8_t PROGIF_PCI_SUBTRACTIVE    = 0x01;

// Simple Communication subclasses
constexpr uint8_t SUB_COMM_SERIAL           = 0x00;
constexpr uint8_t SUB_COMM_PARALLEL         = 0x01;
constexpr uint8_t SUB_COMM_MULTIPORT        = 0x02;
constexpr uint8_t SUB_COMM_MODEM            = 0x03;

// Base System subclasses
constexpr uint8_t SUB_BASE_PIC              = 0x00;
constexpr uint8_t SUB_BASE_DMA              = 0x01;
constexpr uint8_t SUB_BASE_TIMER            = 0x02;
constexpr uint8_t SUB_BASE_RTC              = 0x03;
constexpr uint8_t SUB_BASE_HOTPLUG          = 0x04;
constexpr uint8_t SUB_BASE_SD               = 0x05;
constexpr uint8_t SUB_BASE_IOMMU            = 0x06;

// Input subclasses
constexpr uint8_t SUB_INPUT_KEYBOARD        = 0x00;
constexpr uint8_t SUB_INPUT_PEN             = 0x01;
constexpr uint8_t SUB_INPUT_MOUSE           = 0x02;
constexpr uint8_t SUB_INPUT_SCANNER         = 0x03;
constexpr uint8_t SUB_INPUT_GAMEPORT        = 0x04;

// Serial Bus subclasses
constexpr uint8_t SUB_SERIAL_FIREWIRE       = 0x00;
constexpr uint8_t SUB_SERIAL_USB            = 0x03;
constexpr uint8_t SUB_SERIAL_FIBRE          = 0x04;
constexpr uint8_t SUB_SERIAL_SMBUS          = 0x05;
constexpr uint8_t SUB_SERIAL_INFINIBAND     = 0x06;
constexpr uint8_t SUB_SERIAL_IPMI           = 0x07;

// USB prog IFs
constexpr uint8_t PROGIF_USB_UHCI           = 0x00;
constexpr uint8_t PROGIF_USB_OHCI           = 0x10;
constexpr uint8_t PROGIF_USB_EHCI           = 0x20;
constexpr uint8_t PROGIF_USB_XHCI           = 0x30;
constexpr uint8_t PROGIF_USB_UNSPECIFIED    = 0x80;
constexpr uint8_t PROGIF_USB_DEVICE         = 0xFE;

// Wireless subclasses
constexpr uint8_t SUB_WIRELESS_IRDA         = 0x00;
constexpr uint8_t SUB_WIRELESS_IR           = 0x01;
constexpr uint8_t SUB_WIRELESS_RF           = 0x10;
constexpr uint8_t SUB_WIRELESS_BLUETOOTH    = 0x11;
constexpr uint8_t SUB_WIRELESS_BROADBAND    = 0x12;

/**
 * Get a human-readable name for a PCI device given its class/subclass/prog_if.
 * Returns a static string. Never returns nullptr.
 */
const char* device_class_name(uint8_t class_code, uint8_t subclass, uint8_t prog_if);

} // namespace pci

#endif // STELLUX_PCI_PCI_CLASS_CODES_H
