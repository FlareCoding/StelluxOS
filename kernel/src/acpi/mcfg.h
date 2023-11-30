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

struct PciMsiXCapability {
    uint16_t messageControl;
    uint32_t tableOffset;
    uint32_t pbaOffset; // PBA: Pending Bit Array
} __attribute__((packed));

struct MsiXTableEntry {
    uint64_t messageAddress;
    uint32_t messageData;
    uint32_t vectorControl;
} __attribute__((packed));

struct PciMsiCapability {
    uint16_t messageControl;
    uint32_t messageAddress;
    uint16_t messageData;
} __attribute__((packed));

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
};

__PRIVILEGED_CODE
PciMsiXCapability _readMsixCapability(const uint8_t bus, const uint8_t device, const uint8_t function, uint32_t& capOffset);

__PRIVILEGED_CODE
PciMsiCapability _readMsiCapability(const uint8_t bus, const uint8_t device, const uint8_t function, uint32_t& capOffset);

#endif
