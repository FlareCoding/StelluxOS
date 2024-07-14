#ifndef PCI_H
#define PCI_H
#include <ktypes.h>

#define PCI_CONFIG_ADDRESS                  0xCF8
#define PCI_CONFIG_DATA                     0xCFC

#define PCI_COMMAND_REGISTER                0x04

#define MSI_CAPABILITY_ID                   0x05
#define MSI_X_CAPABILITY_ID                 0x11

#define PCI_CAPABILITY_ID_PMI               0x01
#define PCI_CAPABILITY_ID_AGP               0x02
#define PCI_CAPABILITY_ID_VPD               0x03
#define PCI_CAPABILITY_ID_SLOT_ID           0x04
#define PCI_CAPABILITY_ID_MSI               0x05
#define PCI_CAPABILITY_ID_COMPACTPCI_HS     0x06
#define PCI_CAPABILITY_ID_PCI_X             0x07
#define PCI_CAPABILITY_ID_HYPERTRANSPORT    0x08
#define PCI_CAPABILITY_ID_VENDOR            0x09
#define PCI_CAPABILITY_ID_DEBUG_PORT        0x0A
#define PCI_CAPABILITY_ID_CPCI_RES_CTRL     0x0B
#define PCI_CAPABILITY_ID_HOTPLUG           0x0C
#define PCI_CAPABILITY_ID_BRIDGE_SUBVID     0x0D
#define PCI_CAPABILITY_ID_AGP_8X            0x0E
#define PCI_CAPABILITY_ID_SECURE_DEVICE     0x0F
#define PCI_CAPABILITY_ID_PCI_EXPRESS       0x10
#define PCI_CAPABILITY_ID_MSI_X             0x11
#define PCI_CAPABILITY_ID_SATA_DATA_IDX     0x12
#define PCI_CAPABILITY_ID_PCI_EXPRESS_AF    0x13

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

enum PciCapability {
    PciCapabilityPmi = 0,
    PciCapabilityAgp,
    PciCapabilityVpd,
    PciCapabilitySlotId,
    PciCapabilityMsi,
    PciCapabilityCPHotSwap,
    PciCapabilityPciX,
    PciCapabilityHyperTransport,
    PciCapabilityVendorSpecific,
    PciCapabilityDebugPort,
    PciCapabilityCPCentralResourceControl,
    PciCapabilityPciHotPlug,
    PciCapabilityBridgeSubsystemVendorId,
    PciCapabilityAgp8x,
    PciCapabilitySecureDevice,
    PciCapabilityPciExpress,
    PciCapabilityMsiX,
    PciCapabilitySataConfig,
    PciCapabilityAdvancedFeatures,
    PciCapabilityInvalidCap = 31
};

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

__PRIVILEGED_CODE
void enableBusMastering(uint8_t bus, uint8_t slot, uint8_t func);

__PRIVILEGED_CODE
uint32_t _getPciConfigAddress(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

__PRIVILEGED_CODE
uint8_t pciConfigRead8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

__PRIVILEGED_CODE
uint16_t pciConfigRead16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

__PRIVILEGED_CODE
uint32_t pciConfigRead32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

__PRIVILEGED_CODE
void pciConfigWrite8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t value);

__PRIVILEGED_CODE
void pciConfigWrite16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);

__PRIVILEGED_CODE
void pciConfigWrite32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

#endif
