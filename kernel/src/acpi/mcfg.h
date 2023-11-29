#ifndef MCFG_H
#define MCFG_H
#include "acpi.h"
#include <pci/pci.h>
#include <kvector.h>

struct McfgHeader {
    AcpiTableHeader header;
    uint64_t reserved;
}__attribute__((packed));

struct PciDeviceInfo {
    PciDeviceHeader headerInfo;
    uint64_t        functionAddress;
    uint64_t        barAddress;
    uint8_t         bus;
    uint8_t         device;
    uint8_t         function;
    uint8_t         padding;
    uint32_t        capabilities;
};

#define HAS_PCI_CAP(info, cap) ((bool)(info.capabilities & (1u << cap)))

class Mcfg {
public:
    Mcfg(McfgHeader* table);
    ~Mcfg() = default;

    __PRIVILEGED_CODE
    void enumeratePciDevices();

    inline size_t getDeviceCount() const { return m_devices.size(); }
    inline PciDeviceInfo& getDeviceInfo(size_t idx) { return m_devices[idx]; }

    // Helper functions for finding popular needed PCI devices
    size_t findXhciController();

private:
    McfgHeader* m_base;
    kstl::vector<PciDeviceInfo> m_devices;

private:
    __PRIVILEGED_CODE
    void _enumeratePciFunction(uint64_t deviceAddress, uint64_t function);

    __PRIVILEGED_CODE
    void _enumeratePciDevice(uint64_t busAddress, uint64_t device);

    __PRIVILEGED_CODE
    void _enumeratePciBus(uint64_t baseAddress, uint64_t bus);

    __PRIVILEGED_CODE
    uint32_t _readCapabilities(uint8_t bus, uint8_t device, uint8_t function);

    __PRIVILEGED_CODE
    uint32_t _getPciConfigAddress(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

    __PRIVILEGED_CODE
    uint8_t _pciConfigRead8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
};

#endif
