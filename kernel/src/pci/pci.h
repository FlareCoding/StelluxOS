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

#define MSI_X_TABLE_ENTRY_SIZE 16  // Each MSI-X table entry is 16 bytes

// Mask for extracting the BIR (lower 3 bits) from tableOffset and pbaOffset
#define MSIX_BIR_MASK       0x7        // BIR is stored in bits [2:0]

// Mask for extracting the offset (upper 29 bits) from tableOffset and pbaOffset
#define MSIX_OFFSET_MASK    (~0x7)     // Offset is stored in bits [31:3]

// Macro to extract the BIR (Base Address Register Indicator) from a given offset field
#define MSIX_EXTRACT_BIR(offset)       ((offset) & MSIX_BIR_MASK)

// Macro to extract the Offset (location within the BAR) from a given offset field
#define MSIX_EXTRACT_OFFSET(offset)    ((offset) & MSIX_OFFSET_MASK)

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

//
// https://wiki.osdev.org/PCI#Message_Signaled_Interrupts
//
struct PciMsiXCapability {
    union {
        struct {
            uint8_t capId;
            uint8_t nextCapPtr;
            union {
                struct {
                    // Table Size is N - 1 encoded, and is the number of
                    // entries in the MSI-X table. This field is Read-Only.
                    uint16_t tableSize      : 11;
                    uint16_t rsvd0          : 3;
                    uint16_t functionMask   : 1;
                    uint16_t enableBit      : 1;
                } __attribute__((packed));
                uint16_t messageControl;
            } __attribute__((packed));
        } __attribute__((packed));
        uint32_t dword0;
    };

    union {
        struct {
            // BIR specifies which BAR is used for the Message Table. This may be a 64-bit
            // BAR, and is zero-indexed (so BIR=0, BAR0, offset 0x10 into the header).
            uint32_t tableBir       : 3;
            uint32_t tableOffset    : 29;
        } __attribute__((packed));
        uint32_t dword1;
    };

    union {
        struct {
            // BIR specifies which BAR is used for the Message Table. This may be a 64-bit
            // BAR, and is zero-indexed (so BIR=0, BAR0, offset 0x10 into the header).
            uint32_t pendingBitArrayBir       : 3;
            uint32_t pendingBitArrayOffset    : 29;
        } __attribute__((packed));
        uint32_t dword2;
    };
} __attribute__((packed));
static_assert(sizeof(PciMsiXCapability) == 12);

struct MsiXTableEntry {
    uint32_t messageAddressLo;
    uint32_t messageAddressHi;
    uint32_t messageData;
    uint32_t vectorControl;
} __attribute__((packed));

struct PciMsiCapability {
    union {
        struct {
            uint8_t capId;
            uint8_t nextCapPtr;
            uint16_t messageControl;
        } __attribute__((packed));
        uint32_t dword0;
    };
    uint32_t messageAddress;
    uint16_t messageData;
} __attribute__((packed));

struct PciDeviceInfo {
    PciDeviceHeader headerInfo;
    uint64_t        functionAddress;
    uint64_t        barAddress;
    uint8_t         bus;
    uint8_t         device;
    uint8_t         function;
    uint8_t         padding;
    uint32_t        capabilities;
    uint8_t         msiCapPtr;
    uint8_t         msixCapPtr;
};

const char* getPciDeviceType(uint8_t classCode);
const char* getPciVendorName(uint16_t vendorID);
const char* getPciDeviceName(uint16_t vendorID, uint16_t deviceID);
const char* getPciMassStorageControllerSubclassName(uint8_t subclassCode);
const char* getPciSerialBusControllerSubclassName(uint8_t subclassCode);
const char* getPciBridgeDeviceSubclassName(uint8_t subclassCode);
const char* getPciSubclassName(uint8_t classCode, uint8_t subclassCode);
const char* getPciProgIFName(uint8_t classCode, uint8_t subclassCode, uint8_t progIF);

uint64_t getBarFromPciHeader(PciDeviceHeader* header, size_t barIndex = 0);

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

__PRIVILEGED_CODE
PciMsiXCapability readMsixCapability(const uint8_t bus, const uint8_t device, const uint8_t function);

__PRIVILEGED_CODE
PciMsiCapability readMsiCapability(const uint8_t bus, const uint8_t device, const uint8_t function);

__PRIVILEGED_CODE
void enableMsix(PciMsiXCapability& msixCap, PciDeviceInfo& info);

__PRIVILEGED_CODE
void disableMsix(PciMsiXCapability& msixCap, PciDeviceInfo& info);

__PRIVILEGED_CODE
void disableLegacyInterrupts(PciDeviceInfo& info);

__PRIVILEGED_CODE
void setupMsixIrqRouting(PciDeviceInfo& info, volatile uint16_t** pba);

#endif
