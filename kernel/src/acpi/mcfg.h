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
};

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
};

#endif
