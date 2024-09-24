#include "pci.h"
#include <paging/page.h>
#include <ports/ports.h>
#include <kelevate/kelevate.h>
#include <kprint.h>

const char* g_pciDeviceClasses[] {
    "Unclassified",
    "Mass Storage Controller",
    "Network Controller",
    "Display Controller",
    "Multimedia Controller",
    "Memory Controller",
    "Bridge Device",
    "Simple Communication Controller",
    "Base System Peripheral",
    "Input Device Controller",
    "Docking Station", 
    "Processor",
    "Serial Bus Controller",
    "Wireless Controller",
    "Intelligent Controller",
    "Satellite Communication Controller",
    "Encryption Controller",
    "Signal Processing Controller",
    "Processing Accelerator",
    "Non Essential Instrumentation"
};

const char* getPciDeviceType(uint8_t classCode) {
    return g_pciDeviceClasses[classCode];
}

const char* getPciVendorName(uint16_t vendorID) {
    switch (vendorID) {
        case 0x8086:
            return "Intel Corp";
        case 0x1022:
            return "AMD";
        case 0x10DE:
            return "NVIDIA Corporation";
        default:
            break;
    }

    return "Unknown Vendor ID";
}

const char* getPciDeviceName(uint16_t vendorID, uint16_t deviceID) {
    switch (vendorID) {
        case 0x8086: { // Intel
            switch(deviceID) {
                case 0x29C0:
                    return "Express DRAM Controller";
                case 0x2918:
                    return "LPC Interface Controller";
                case 0x2922:
                    return "6 port SATA Controller [AHCI mode]";
                case 0x2930:
                    return "SMBus Controller";
                default:
                    break;
            }
        }
    }
    return "Unknown Device ID";
}

const char* getPciMassStorageControllerSubclassName(uint8_t subclassCode) {
    switch (subclassCode) {
        case 0x00:
            return "SCSI Bus Controller";
        case 0x01:
            return "IDE Controller";
        case 0x02:
            return "Floppy Disk Controller";
        case 0x03:
            return "IPI Bus Controller";
        case 0x04:
            return "RAID Controller";
        case 0x05:
            return "ATA Controller";
        case 0x06:
            return "Serial ATA";
        case 0x07:
            return "Serial Attached SCSI";
        case 0x08:
            return "Non-Volatile Memory Controller";
        case 0x80:
            return "Other";
        default:
            break;
    }

    return "Unknown Subclass Code";
}

const char* getPciSerialBusControllerSubclassName(uint8_t subclassCode) {
    switch (subclassCode) {
        case 0x00:
            return "FireWire (IEEE 1394) Controller";
        case 0x01:
            return "ACCESS Bus";
        case 0x02:
            return "SSA";
        case 0x03:
            return "USB Controller";
        case 0x04:
            return "Fibre Channel";
        case 0x05:
            return "SMBus";
        case 0x06:
            return "Infiniband";
        case 0x07:
            return "IPMI Interface";
        case 0x08:
            return "SERCOS Interface (IEC 61491)";
        case 0x09:
            return "CANbus";
        case 0x80:
            return "SerialBusController - Other";
        default:
            break;
    }
    
    return "Unknown Subclass Code";
}

const char* getPciBridgeDeviceSubclassName(uint8_t subclassCode) {
    switch (subclassCode) {
        case 0x00:
            return "Host Bridge";
        case 0x01:
            return "ISA Bridge";
        case 0x02:
            return "EISA Bridge";
        case 0x03:
            return "MCA Bridge";
        case 0x04:
            return "PCI-to-PCI Bridge";
        case 0x05:
            return "PCMCIA Bridge";
        case 0x06:
            return "NuBus Bridge";
        case 0x07:
            return "CardBus Bridge";
        case 0x08:
            return "RACEway Bridge";
        case 0x09:
            return "PCI-to-PCI Bridge";
        case 0x0a:
            return "InfiniBand-to-PCI Host Bridge";
        case 0x80:
            return "Other";
        default:
            break;
    }
    
    return "Unknown Subclass Code";
}

const char* getPciSubclassName(uint8_t classCode, uint8_t subclassCode) {
    switch (classCode) {
        case 0x01:
            return getPciMassStorageControllerSubclassName(subclassCode);
        case 0x03: {
            switch (subclassCode) {
                case 0x00:
                    return "VGA Compatible Controller";
                default:
                    break;
            }
            break;
        }
        case 0x06:
            return getPciBridgeDeviceSubclassName(subclassCode);
        case 0x0C:
            return getPciSerialBusControllerSubclassName(subclassCode);
        default:
            break;
    }
    
    return "Unknown Subclass Code";
}

const char* getPciProgIFName(uint8_t classCode, uint8_t subclassCode, uint8_t progIF) {
    switch (classCode){
        case 0x01: {
            switch (subclassCode) {
                case 0x06: {
                    switch (progIF) {
                        case 0x00:
                            return "Vendor Specific Interface";
                        case 0x01:
                            return "AHCI 1.0";
                        case 0x02:
                            return "Serial Storage Bus";
                        default:
                            break;
                    }
                    break;
                }
                default:
                    break;
            }
            break;
        }
        case 0x03: {
            switch (subclassCode) {
                case 0x00: {
                    switch (progIF){
                        case 0x00:
                            return "VGA Controller";
                        case 0x01:
                            return "8514-Compatible Controller";
                        default:
                            break;
                    }
                    break;
                }
                default:
                    break;
            }
            break;
        }
        case 0x0C: {
            switch (subclassCode) {
                case 0x03: {
                    switch (progIF) {
                        case 0x00:
                            return "UHCI Controller";
                        case 0x10:
                            return "OHCI Controller";
                        case 0x20:
                            return "EHCI (USB2) Controller";
                        case 0x30:
                            return "XHCI (USB3) Controller";
                        case 0x80:
                            return "Unspecified";
                        case 0xFE:
                            return "USB Device (Not a Host Controller)";
                        default:
                            break;
                    }
                    break;
                }
                default:
                    break;
            }
            break;
        }
        default:
            break;
    }
    
    return "Unknown Controller";
}

uint64_t getBarFromPciHeader(PciDeviceHeader* header) {
    for (int i = 0; i < 2; i++) {
        uint32_t barValue = header->bar[i];

        // Check if the BAR is memory-mapped
        if ((barValue & 0x1) == 0) {
            // Check if the BAR is 64-bit (by checking the type in bits [1:2])
            if ((barValue & 0x6) == 0x4) {
                // It's a 64-bit BAR, read the high part from the next BAR
                uint64_t high = (uint64_t)(header->bar[i + 1]);
                uint64_t address = (high << 32) | (barValue & ~0xF);
                return address;
            } else {
                // It's a 32-bit BAR
                uint64_t address = (uint64_t)(barValue & ~0xF);
                return address;
            }
        }
    }

    return 0; // No valid BAR found
}

void dbgPrintPciDeviceInfo(PciDeviceHeader* header) {
    kuPrint(
        "%s / %s / %s / %s / %s\n",
        getPciVendorName(header->vendorID),
        getPciDeviceName(header->vendorID, header->deviceID),
        getPciDeviceType(header->classCode),
        getPciSubclassName(header->classCode, header->subclass),
        getPciProgIFName(header->classCode, header->subclass, header->progIF)
    );
}

__PRIVILEGED_CODE
void enableBusMastering(uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t commandReg = pciConfigRead16(bus, slot, func, PCI_COMMAND_REGISTER);
    commandReg |= 0x04;  // Set Bus Master Enable bit (bit 2)
    pciConfigWrite16(bus, slot, func, PCI_COMMAND_REGISTER, commandReg);
}

__PRIVILEGED_CODE
uint32_t _getPciConfigAddress(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return ((uint32_t)(bus) << 16) | ((uint32_t)(slot) << 11) |
           ((uint32_t)(func) << 8) | (offset & 0xfc) | ((uint32_t)0x80000000);
}

__PRIVILEGED_CODE
uint8_t pciConfigRead8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, _getPciConfigAddress(bus, slot, func, offset));
    uint32_t data = inl(PCI_CONFIG_DATA);
    return (data >> ((offset & 3) * 8)) & 0xff;
}

__PRIVILEGED_CODE
uint16_t pciConfigRead16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, _getPciConfigAddress(bus, slot, func, offset));
    uint32_t data = inl(PCI_CONFIG_DATA);
    return (data >> ((offset & 2) * 8)) & 0xffff;
}

__PRIVILEGED_CODE
uint32_t pciConfigRead32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, _getPciConfigAddress(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

__PRIVILEGED_CODE
void pciConfigWrite8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t value) {
    outl(PCI_CONFIG_ADDRESS, _getPciConfigAddress(bus, slot, func, offset));
    uint32_t data = inl(PCI_CONFIG_DATA);
    data &= ~(0xFF << ((offset & 3) * 8)); // Clear the byte
    data |= (uint32_t)value << ((offset & 3) * 8); // Set the new value
    outl(PCI_CONFIG_DATA, data);
}

__PRIVILEGED_CODE
void pciConfigWrite16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value) {
    outl(PCI_CONFIG_ADDRESS, _getPciConfigAddress(bus, slot, func, offset));
    uint32_t data = inl(PCI_CONFIG_DATA);
    data &= ~(0xFFFF << ((offset & 2) * 8)); // Clear the 16-bit word
    data |= (uint32_t)value << ((offset & 2) * 8); // Set the new value
    outl(PCI_CONFIG_DATA, data);
}

__PRIVILEGED_CODE
void pciConfigWrite32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, _getPciConfigAddress(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

__PRIVILEGED_CODE
PciMsiXCapability readMsixCapability(const uint8_t bus, const uint8_t device, const uint8_t function) {
    PciMsiXCapability msixCap;
    uint8_t capPointer = pciConfigRead8(bus, device, function, offsetof(PciDeviceHeader, capabilitiesPtr));

    while (capPointer != 0 && capPointer != 0xFF) {
        uint8_t capId = pciConfigRead8(bus, device, function, capPointer);
        if (capId == PCI_CAPABILITY_ID_MSI_X) {
            // Read the MSI-X capability structure
            msixCap.messageControl = pciConfigRead16(bus, device, function, capPointer + offsetof(PciMsiXCapability, messageControl));
            msixCap.tableOffset = pciConfigRead32(bus, device, function, capPointer + offsetof(PciMsiXCapability, tableOffset));
            msixCap.pbaOffset = pciConfigRead32(bus, device, function, capPointer + offsetof(PciMsiXCapability, pbaOffset));
            break;
        }
        capPointer = pciConfigRead8(bus, device, function, capPointer + 1);
    }

    return msixCap;
}

__PRIVILEGED_CODE
PciMsiCapability readMsiCapability(const uint8_t bus, const uint8_t device, const uint8_t function) {
    PciMsiCapability msiCap;
    uint8_t capPointer = pciConfigRead8(bus, device, function, offsetof(PciDeviceHeader, capabilitiesPtr));

    while (capPointer != 0 && capPointer != 0xFF) {
        uint8_t capId = pciConfigRead8(bus, device, function, capPointer);
        if (capId == PCI_CAPABILITY_ID_MSI) {
            // Read the MSI capability structure
            msiCap.messageControl = pciConfigRead16(bus, device, function, capPointer + offsetof(PciMsiCapability, messageControl));
            msiCap.messageAddress = pciConfigRead32(bus, device, function, capPointer + offsetof(PciMsiCapability, messageAddress));
            msiCap.messageData = pciConfigRead16(bus, device, function, capPointer + offsetof(PciMsiCapability, messageData));
            break;
        }
        capPointer = pciConfigRead8(bus, device, function, capPointer + 1);
    }

    return msiCap;
}

bool isMsixCapabilityEnabled(PciDeviceInfo& info) {
    // MSI-X is supported, read MSI-X capability
    PciMsiXCapability msixCap;
    RUN_ELEVATED({
        msixCap = readMsixCapability(info.bus, info.device, info.function);
    });
    
    // Check if MSI-X is enabled
    return static_cast<bool>((msixCap.messageControl & (1 << 15)));
}

bool isMsiCapabilityEnabled(PciDeviceInfo& info) {
    // MSI is supported, read MSI capability
    PciMsiCapability msiCap;
    RUN_ELEVATED({
        msiCap = readMsiCapability(info.bus, info.device, info.function);
    });
    
    // Check if MSI is enabled
    return static_cast<bool>((msiCap.messageControl & (1 << 0)));
}
