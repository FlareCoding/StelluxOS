#ifndef PCI_H
#define PCI_H
#include <ktypes.h>

struct PciDeviceHeader {
    uint16_t vendorID;
    uint16_t deviceID;
    uint16_t command;
    uint16_t status;
    uint8_t revisionID;
    uint8_t progIF;
    uint8_t subclass;
    uint8_t classCode;
    uint8_t cacheLineSize;
    uint8_t latencyTimer;
    uint8_t headerType;
    uint8_t bist;
    uint32_t bar[6];
    uint32_t cardbusCISPtr;
    uint16_t subsystemVendorID;
    uint16_t subsystemID;
    uint32_t expansionROMBaseAddr;
    uint8_t capabilitiesPtr;
    uint8_t reserved[7];
    uint8_t interruptLine;
    uint8_t interruptPin;
    uint8_t minGrant;
    uint8_t maxLatency;
};

struct PciDeviceConfig {
    uint64_t base;
    uint16_t pciSegGroup;
    uint8_t startBus;
    uint8_t endBus;
    uint32_t reserved;
}__attribute__((packed));

const char* getPciDeviceType(uint8_t classCode);
const char* getPciVendorName(uint16_t vendorID);
const char* getPciDeviceName(uint16_t vendorID, uint16_t deviceID);
const char* getPciMassStorageControllerSubclassName(uint8_t subclassCode);
const char* getPciSerialBusControllerSubclassName(uint8_t subclassCode);
const char* getPciBridgeDeviceSubclassName(uint8_t subclassCode);
const char* getPciSubclassName(uint8_t classCode, uint8_t subclassCode);
const char* getPciProgIFName(uint8_t classCode, uint8_t subclassCode, uint8_t progIF);

uint64_t getBarFromPciHeader(PciDeviceHeader* header);

void dbgPrintPciDeviceInfo(PciDeviceHeader* header);

#endif
