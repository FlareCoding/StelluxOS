#include "pci.h"
#include <paging/page.h>
#include <memory/kmemory.h>
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

uint64_t getBarFromPciHeader(PciDeviceHeader* header, size_t barIndex) {
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

void dbgPrintPciDeviceInfo(PciDeviceHeader* header) {
    kprintf(
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
            msixCap.dword0 = pciConfigRead32(bus, device, function, capPointer + 0x00);
            msixCap.dword1 = pciConfigRead32(bus, device, function, capPointer + 0x04);
            msixCap.dword2 = pciConfigRead32(bus, device, function, capPointer + 0x08);
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
            msiCap.capId = capId;
            msiCap.nextCapPtr = pciConfigRead8(bus, device, function, capPointer + 1);
            msiCap.messageControl = pciConfigRead16(bus, device, function, capPointer + 2);

            uint8_t offset = capPointer + 4;

            bool is64Bit = (msiCap._64bit != 0);
            bool perVectorMasking = (msiCap.perVectorMasking != 0);

            // Read message address
            msiCap.messageAddressLo = pciConfigRead32(bus, device, function, offset);
            offset += 4;

            if (is64Bit) {
                msiCap.messageAddressHi = pciConfigRead32(bus, device, function, offset);
                offset += 4;
            } else {
                msiCap.messageAddressHi = 0;
            }

            // Read message data
            msiCap.messageData = pciConfigRead16(bus, device, function, offset);
            offset += 2;

            // Read mask and pending bits if per-vector masking is supported
            if (perVectorMasking) {
                msiCap.mask = pciConfigRead32(bus, device, function, offset);
                offset += 4;
                msiCap.pending = pciConfigRead32(bus, device, function, offset);
                offset += 4;
            } else {
                msiCap.mask = 0;
                msiCap.pending = 0;
            }

            break;
        }
        capPointer = pciConfigRead8(bus, device, function, capPointer + 1);
    }

    return msiCap;
}

__PRIVILEGED_CODE
void enableMsi(PciMsiCapability& msiCap, PciDeviceInfo& info) {
    // Set the MSI enable bit
    msiCap.enableBit = 1;

    uint16_t control = msiCap.messageControl;

    // Write the updated control back to the device's configuration space
    pciConfigWrite16(info.bus, info.device, info.function, info.msiCapPtr + offsetof(PciMsiCapability, messageControl), control);
}

__PRIVILEGED_CODE
void disableMsi(PciMsiCapability& msiCap, PciDeviceInfo& info) {
    // Set the MSI enable bit
    msiCap.enableBit = 0;

    uint16_t control = msiCap.messageControl;

    // Write the updated control back to the device's configuration space
    pciConfigWrite16(info.bus, info.device, info.function, info.msiCapPtr + offsetof(PciMsiCapability, messageControl), control);
}

__PRIVILEGED_CODE
void enableMsix(PciMsiXCapability& msixCap, PciDeviceInfo& info) {
    // Set the MSI-X enable bit (bit 15)
    msixCap.enableBit = 1;

    uint16_t control = msixCap.messageControl;

    // Write the updated control back to the device's configuration space
    pciConfigWrite16(info.bus, info.device, info.function, info.msixCapPtr + offsetof(PciMsiXCapability, messageControl), control);
}

__PRIVILEGED_CODE
void disableMsix(PciMsiXCapability& msixCap, PciDeviceInfo& info) {
    // Set the MSI-X enable bit (bit 15)
    msixCap.enableBit = 0;

    uint16_t control = msixCap.messageControl;

    // Write the updated control back to the device's configuration space
    pciConfigWrite16(info.bus, info.device, info.function, info.msixCapPtr + offsetof(PciMsiXCapability, messageControl), control);
}

__PRIVILEGED_CODE
void disableLegacyInterrupts(PciDeviceInfo& info) {
    // Read the current Command register from PCI configuration space
    uint16_t command = pciConfigRead16(info.bus, info.device, info.function, 0x04); // Offset 0x04 is the Command register

    // Set bit 10 (Interrupt Disable) to 1 to disable legacy interrupts
    command |= (1 << 10);

    // Write the modified Command register back to PCI configuration space
    pciConfigWrite16(info.bus, info.device, info.function, 0x04, command);
}

uint64_t x86archMsiAddress(uint64_t *data, size_t vector, uint32_t processor, uint8_t edgetrigger, uint8_t deassert) {
	*data = (vector & 0xFF) | (edgetrigger == 1 ? 0 : (1 << 15)) | (deassert == 1 ? 0 : (1 << 14));
	return (0xfee00000 | (processor << 12));
}

__PRIVILEGED_CODE
void setupMsixIrqRouting(PciDeviceInfo& info, volatile uint16_t** pba) {
    PciMsiXCapability msixCap = readMsixCapability(info.bus, info.device, info.function);

    kprintf("msix.tableSize    : %i, (%i: 1-indexed)\n", msixCap.tableSize, msixCap.tableSize + 1);
    kprintf("msix.functionMask : %i\n", msixCap.functionMask);
    kprintf("msix.enableBit    : %i\n", msixCap.enableBit);
    kprintf("msix.tableBir     : %i\n", msixCap.tableBir);
    kprintf("msix.tableOffset  : 0x%llx\n", msixCap.tableOffset);
    kprintf("msix.pbaBir       : %i\n", msixCap.pendingBitArrayBir);
    kprintf("msix.pbaOffset    : 0x%llx\n\n", msixCap.pendingBitArrayOffset);

    uint64_t tableBar = getBarFromPciHeader(&info.headerInfo, msixCap.tableBir);
    uint64_t tableAddress = (tableBar) + msixCap.tableOffset;
    kprintf("msix-Table Address: 0x%llx\n", tableAddress);

    uint64_t pbaAddress = (tableBar) + msixCap.pendingBitArrayOffset;
    kprintf("msix-PBA Address: 0x%llx\n", pbaAddress);

    uint32_t tableEntryCount = msixCap.tableSize + 1;
    kprintf("msix-Table Entries: %i\n", tableEntryCount);

    volatile MsiXTableEntry* msixTable = (volatile MsiXTableEntry*)zallocPage();
    paging::mapPage((void*)msixTable,(void*)tableAddress, KERNEL_PAGE, PAGE_ATTRIB_CACHE_DISABLED, paging::getCurrentTopLevelPageTable());

    uint64_t msi_data = 0;
    uint64_t msi_addr = x86archMsiAddress(&msi_data, IRQ2, BSP_CPU_ID, 1, 0);
    __unused msi_addr;

    for (uint32_t i = 0; i < tableEntryCount; i++) {
        msixTable[i].messageAddressLo = 0xfee00000;
        msixTable[i].messageAddressHi = 0x00000000;
        msixTable[i].messageData = msi_data;
        msixTable[i].vectorControl = 0; // Vector control: unmasked
    }
    
    volatile uint16_t* pbaVirtualAddress = (volatile uint16_t*)zallocPage();
    paging::mapPage((void*)pbaVirtualAddress,(void*)pbaAddress, KERNEL_PAGE, PAGE_ATTRIB_CACHE_DISABLED, paging::getCurrentTopLevelPageTable());

    *pba = pbaVirtualAddress;

    enableMsix(msixCap, info);
    kprintf("Enabled MSI-X capability\n");
}

__PRIVILEGED_CODE
bool setupMsiInterrupt(PciDeviceInfo& deviceInfo, uint8_t interruptVector, uint8_t cpu) {
    uint8_t bus = deviceInfo.bus;
    uint8_t device = deviceInfo.device;
    uint8_t function = deviceInfo.function;

    PciMsiCapability msiCap = readMsiCapability(bus, device, function);
    uint8_t msiCapPtr = deviceInfo.msiCapPtr;

    // Determine if the device supports 64-bit addresses
    bool is64Bit = msiCap._64bit != 0;

    // Determine if per-vector masking is supported
    bool perVectorMasking = msiCap.perVectorMasking != 0;

    // Calculate the MSI message address and data using the provided function
    uint64_t messageData = 0;
    uint64_t messageAddress = x86archMsiAddress(&messageData, interruptVector, cpu, 1, 1); // Edge-triggered, deasserted

    // Write the message address and data to the device's MSI capability
    uint8_t offset = msiCapPtr + 4;

    // Write message address lower 32 bits
    pciConfigWrite32(bus, device, function, offset, (uint32_t)(messageAddress & 0xFFFFFFFF));
    offset += 4;

    if (is64Bit) {
        // Write message address upper 32 bits
        pciConfigWrite32(bus, device, function, offset, (uint32_t)(messageAddress >> 32));
        offset += 4;
    }

    // Write message data
    pciConfigWrite16(bus, device, function, offset, (uint16_t)(messageData & 0xFFFF));
    offset += 2;

    // If per-vector masking is supported, write mask and pending bits
    if (perVectorMasking) {
        // Unmask all vectors (set mask bits to 0)
        pciConfigWrite32(bus, device, function, offset, 0x0);
        offset += 4;

        // Clear all pending bits
        pciConfigWrite32(bus, device, function, offset, 0x0);
        offset += 4;
    }

    // Enable MSI by setting the MSI Enable bit in the Message Control register
    msiCap.enableBit = 1;

    // Write back the updated Message Control register
    pciConfigWrite16(bus, device, function, msiCapPtr + 2, msiCap.messageControl);

    return true;
}
