#include "mcfg.h"
#include <paging/page.h>
#include <paging/phys_addr_translation.h>
#include <interrupts/interrupts.h>

Mcfg::Mcfg(McfgHeader* table) {
    m_base = (McfgHeader*)__va(table);
}

__PRIVILEGED_CODE
void Mcfg::enumeratePciDevices() {
    int entries = ((m_base->header.length) - sizeof(McfgHeader)) / sizeof(PciDeviceConfig);
    for (int t = 0; t < entries; t++) {
        PciDeviceConfig* newDeviceConfig = (PciDeviceConfig*)((uint64_t)m_base + sizeof(McfgHeader) + (sizeof(PciDeviceConfig) * t));
        for (uint64_t bus = newDeviceConfig->startBus; bus < newDeviceConfig->endBus; bus++) {
            _enumeratePciBus(newDeviceConfig->base, bus);
        }
    }
}

size_t Mcfg::findXhciController() {
    for (size_t i = 0; i < m_devices.size(); i++) {
        auto& info = m_devices[i];
        if (
            info.headerInfo.classCode == 0x0C &&
            info.headerInfo.subclass == 0x03 &&
            info.headerInfo.progIF == 0x30
        ) {
            return i;
        }
    }

    return kstl::npos;
}

__PRIVILEGED_CODE
void Mcfg::_enumeratePciFunction(uint64_t deviceAddress, uint64_t function) {
    uint64_t offset = function << 12;

    uint64_t functionAddress = deviceAddress + offset;
    paging::mapPage((void*)functionAddress, (void*)functionAddress, KERNEL_PAGE, paging::getCurrentTopLevelPageTable());

    volatile PciDeviceHeader* pciDeviceHeader = (volatile PciDeviceHeader*)functionAddress;

    if (pciDeviceHeader->deviceID == 0) return;
    if (pciDeviceHeader->deviceID == 0xFFFF) return;

    PciDeviceInfo info;
    zeromem(&info, sizeof(PciDeviceInfo));

    for (size_t i = 0; i < sizeof(PciDeviceHeader); i++) {
        volatile uint8_t* src = ((volatile uint8_t*)pciDeviceHeader) + i;
        uint8_t* dest = ((uint8_t*)&info.headerInfo) + i;

        *dest = *src;
    }

    info.functionAddress = functionAddress;
    info.barAddress = getBarFromPciHeader(&info.headerInfo);
    info.bus = (deviceAddress >> 20) & 0xFF;
    info.device = (deviceAddress >> 15) & 0x1F;
    info.function = (uint8_t)function;
    info.capabilities = _readCapabilities(info.bus, info.device, info.function);

    m_devices.pushBack(info);
}

__PRIVILEGED_CODE
void Mcfg::_enumeratePciDevice(uint64_t busAddress, uint64_t device) {
    uint64_t offset = device << 15;

    uint64_t deviceAddress = busAddress + offset;
    paging::mapPage((void*)deviceAddress, (void*)deviceAddress, KERNEL_PAGE, paging::getCurrentTopLevelPageTable());

    PciDeviceHeader* pciDeviceHeader = (PciDeviceHeader*)deviceAddress;

    if (pciDeviceHeader->deviceID == 0) return;
    if (pciDeviceHeader->deviceID == 0xFFFF) return;

    for (uint64_t function = 0; function < 8; function++){
        _enumeratePciFunction(deviceAddress, function);
    }
}

__PRIVILEGED_CODE
void Mcfg::_enumeratePciBus(uint64_t baseAddress, uint64_t bus) {
    uint64_t offset = bus << 20;

    uint64_t busAddress = baseAddress + offset;
    paging::mapPage((void*)busAddress, (void*)busAddress, KERNEL_PAGE, paging::getCurrentTopLevelPageTable());

    PciDeviceHeader* pciDeviceHeader = (PciDeviceHeader*)busAddress;

    if (pciDeviceHeader->deviceID == 0) return;
    if (pciDeviceHeader->deviceID == 0xFFFF) return;

    for (uint64_t device = 0; device < 32; device++){
        _enumeratePciDevice(busAddress, device);
    }
}

__PRIVILEGED_CODE
uint32_t Mcfg::_readCapabilities(uint8_t bus, uint8_t device, uint8_t function) {
    uint32_t capabilities;

    uint8_t capPointer = pciConfigRead8(bus, device, function, 0x34);

    while (capPointer != 0 && capPointer != 0xFF) {
        uint8_t capId = pciConfigRead8(bus, device, function, capPointer);
        PciCapability cap = PciCapabilityInvalidCap;

        switch (capId) {
            case PCI_CAPABILITY_ID_PMI: 
                cap = PciCapabilityPmi;
                break;
            case PCI_CAPABILITY_ID_AGP: 
                cap = PciCapabilityAgp;
                break;
            case PCI_CAPABILITY_ID_VPD: 
                cap = PciCapabilityVpd;
                break;
            case PCI_CAPABILITY_ID_SLOT_ID: 
                cap = PciCapabilitySlotId;
                break;
            case PCI_CAPABILITY_ID_MSI: 
                cap = PciCapabilityMsi;
                break;
            case PCI_CAPABILITY_ID_COMPACTPCI_HS: 
                cap = PciCapabilityCPHotSwap;
                break;
            case PCI_CAPABILITY_ID_PCI_X: 
                cap = PciCapabilityPciX;
                break;
            case PCI_CAPABILITY_ID_HYPERTRANSPORT: 
                cap = PciCapabilityHyperTransport;
                break;
            case PCI_CAPABILITY_ID_VENDOR: 
                cap = PciCapabilityVendorSpecific;
                break;
            case PCI_CAPABILITY_ID_DEBUG_PORT: 
                cap = PciCapabilityDebugPort;
                break;
            case PCI_CAPABILITY_ID_CPCI_RES_CTRL: 
                cap = PciCapabilityCPCentralResourceControl;
                break;
            case PCI_CAPABILITY_ID_HOTPLUG: 
                cap = PciCapabilityPciHotPlug;
                break;
            case PCI_CAPABILITY_ID_BRIDGE_SUBVID: 
                cap = PciCapabilityBridgeSubsystemVendorId;
                break;
            case PCI_CAPABILITY_ID_AGP_8X: 
                cap = PciCapabilityAgp8x;
                break;
            case PCI_CAPABILITY_ID_SECURE_DEVICE: 
                cap = PciCapabilitySecureDevice;
                break;
            case PCI_CAPABILITY_ID_PCI_EXPRESS: 
                cap = PciCapabilityPciExpress;
                break;
            case PCI_CAPABILITY_ID_MSI_X: 
                cap = PciCapabilityMsiX;
                break;
            case PCI_CAPABILITY_ID_SATA_DATA_IDX: 
                cap = PciCapabilitySataConfig;
                break;
            case PCI_CAPABILITY_ID_PCI_EXPRESS_AF: 
                cap = PciCapabilityAdvancedFeatures;
                break;
            default: 
                break;
        }

        capPointer = pciConfigRead8(bus, device, function, capPointer + 1);

        if (cap != PciCapabilityInvalidCap) {
            capabilities |= (1u << cap);
        }
    }

    return capabilities;
}

__PRIVILEGED_CODE
PciMsiXCapability _readMsixCapability(const uint8_t bus, const uint8_t device, const uint8_t function, uint32_t& capOffset) {
    PciMsiXCapability msixCap;
    uint8_t capPointer = pciConfigRead8(bus, device, function, offsetof(PciDeviceHeader, capabilitiesPtr));

    while (capPointer != 0 && capPointer != 0xFF) {
        uint8_t capId = pciConfigRead8(bus, device, function, capPointer);
        if (capId == PCI_CAPABILITY_ID_MSI_X) {
            // Read the MSI-X capability structure
            msixCap.messageControl = pciConfigRead16(bus, device, function, capPointer + offsetof(PciMsiXCapability, messageControl));
            msixCap.tableOffset = pciConfigRead32(bus, device, function, capPointer + offsetof(PciMsiXCapability, tableOffset));
            msixCap.pbaOffset = pciConfigRead32(bus, device, function, capPointer + offsetof(PciMsiXCapability, pbaOffset));

            capOffset = capPointer;
            break;
        }
        capPointer = pciConfigRead8(bus, device, function, capPointer + 1);
    }

    return msixCap;
}

__PRIVILEGED_CODE
PciMsiCapability _readMsiCapability(const uint8_t bus, const uint8_t device, const uint8_t function, uint32_t& capOffset) {
    PciMsiCapability msiCap;
    uint8_t capPointer = pciConfigRead8(bus, device, function, offsetof(PciDeviceHeader, capabilitiesPtr));

    while (capPointer != 0 && capPointer != 0xFF) {
        uint8_t capId = pciConfigRead8(bus, device, function, capPointer);
        if (capId == PCI_CAPABILITY_ID_MSI) {
            // Read the MSI capability structure
            msiCap.messageControl = pciConfigRead16(bus, device, function, capPointer + offsetof(PciMsiCapability, messageControl));
            msiCap.messageAddress = pciConfigRead32(bus, device, function, capPointer + offsetof(PciMsiCapability, messageAddress));
            msiCap.messageData = pciConfigRead16(bus, device, function, capPointer + offsetof(PciMsiCapability, messageData));

            capOffset = capPointer;
            break;
        }
        capPointer = pciConfigRead8(bus, device, function, capPointer + 1);
    }

    return msiCap;
}
