#include "pci.h"
#include <paging/page.h>
#include <drivers/usb/xhci.h>
#include <kprint.h>

const char* g_deviceClasses[] {
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

const char* getVendorName(uint16_t vendorID){
    switch (vendorID){
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

const char* getDeviceName(uint16_t vendorID, uint16_t deviceID){
    switch (vendorID){
        case 0x8086: // Intel
            switch(deviceID){
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
    return "Unknown Device ID";
}

const char* massStorageControllerSubclassName(uint8_t subclassCode){
    switch (subclassCode){
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

const char* serialBusControllerSubclassName(uint8_t subclassCode){
    switch (subclassCode){
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

const char* bridgeDeviceSubclassName(uint8_t subclassCode){
    switch (subclassCode){
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

const char* getSubclassName(uint8_t classCode, uint8_t subclassCode){
    switch (classCode) {
        case 0x01:
            return massStorageControllerSubclassName(subclassCode);
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
            return bridgeDeviceSubclassName(subclassCode);
        case 0x0C:
            return serialBusControllerSubclassName(subclassCode);
        default:
            break;
    }
    
    return "Unknown Subclass Code";
}

const char* getProgIFName(uint8_t classCode, uint8_t subclassCode, uint8_t progIF){
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

uint64_t getBarFromHeader(PciDeviceHeader* header) {
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

void enumeratePciFunction(uint64_t deviceAddress, uint64_t function) {
    uint64_t offset = function << 12;

    uint64_t functionAddress = deviceAddress + offset;
    paging::mapPage((void*)functionAddress, (void*)functionAddress, KERNEL_PAGE, paging::getCurrentTopLevelPageTable());

    PciDeviceHeader* pciDeviceHeader = (PciDeviceHeader*)functionAddress;

    if (pciDeviceHeader->deviceID == 0) return;
    if (pciDeviceHeader->deviceID == 0xFFFF) return;

    if (
        pciDeviceHeader->classCode == 0x0C &&
        pciDeviceHeader->subclass == 0x03 &&
        pciDeviceHeader->progIF == 0x30
    ) {
        kuPrint(
            "       FOUND %s - %s - %s\n",
            g_deviceClasses[pciDeviceHeader->classCode],
            getSubclassName(pciDeviceHeader->classCode, pciDeviceHeader->subclass),
            getProgIFName(pciDeviceHeader->classCode, pciDeviceHeader->subclass, pciDeviceHeader->progIF)
        );

        // Extract the BAR (Base Address Register)
        uint64_t bar = getBarFromHeader(pciDeviceHeader); // You'll need to implement getBarFromHeader

        kuPrint("           BAR: 0x%llx\n", bar);
        
        // Call the initialization function for xHCI controller
        xhciControllerInit(bar);
    }

    // kuPrint(
    //     "           %s / %s / %s / %s / %s\n",
    //     getVendorName(pciDeviceHeader->vendorID),
    //     getDeviceName(pciDeviceHeader->vendorID, pciDeviceHeader->deviceID),
    //     g_deviceClasses[pciDeviceHeader->classCode],
    //     getSubclassName(pciDeviceHeader->classCode, pciDeviceHeader->subclass),
    //     getProgIFName(pciDeviceHeader->classCode, pciDeviceHeader->subclass, pciDeviceHeader->progIF)
    // );
}

void enumeratePciDevice(uint64_t busAddress, uint64_t device) {
    uint64_t offset = device << 15;

    uint64_t deviceAddress = busAddress + offset;
    paging::mapPage((void*)deviceAddress, (void*)deviceAddress, KERNEL_PAGE, paging::getCurrentTopLevelPageTable());

    PciDeviceHeader* pciDeviceHeader = (PciDeviceHeader*)deviceAddress;

    if (pciDeviceHeader->deviceID == 0) return;
    if (pciDeviceHeader->deviceID == 0xFFFF) return;

    for (uint64_t function = 0; function < 8; function++){
        enumeratePciFunction(deviceAddress, function);
    }
}

void enumeratePciBus(uint64_t baseAddress, uint64_t bus) {
    uint64_t offset = bus << 20;

    uint64_t busAddress = baseAddress + offset;
    paging::mapPage((void*)busAddress, (void*)busAddress, KERNEL_PAGE, paging::getCurrentTopLevelPageTable());

    PciDeviceHeader* pciDeviceHeader = (PciDeviceHeader*)busAddress;

    if (pciDeviceHeader->deviceID == 0) return;
    if (pciDeviceHeader->deviceID == 0xFFFF) return;

    for (uint64_t device = 0; device < 32; device++){
        enumeratePciDevice(busAddress, device);
    }
}

void enumeratePciDevices(McfgHeader* mcfg) {
    int entries = ((mcfg->header.length) - sizeof(McfgHeader)) / sizeof(PciDeviceConfig);
    for (int t = 0; t < entries; t++){
        PciDeviceConfig* newDeviceConfig = (PciDeviceConfig*)((uint64_t)mcfg + sizeof(McfgHeader) + (sizeof(PciDeviceConfig) * t));
        for (uint64_t bus = newDeviceConfig->startBus; bus < newDeviceConfig->endBus; bus++){
            enumeratePciBus(newDeviceConfig->base, bus);
        }
    }
}
