#include "mcfg.h"
#include <paging/page.h>
#include <paging/phys_addr_translation.h>

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
