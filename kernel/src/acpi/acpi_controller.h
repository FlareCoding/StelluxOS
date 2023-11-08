#ifndef ACPI_CONTROLLER_H
#define ACPI_CONTROLLER_H
#include <ktypes.h>

// Generic Address Structure (GAS) as defined in the ACPI specification
struct GenericAddressStructure {
    uint8_t  addressSpace;
    uint8_t  bitWidth;
    uint8_t  bitOffset;
    uint8_t  accessSize;
    uint64_t address;
} __attribute__((packed));

// ACPI RSDP (Root System Description Pointer)
struct AcpiRsdp {
    char     signature[8];
    uint8_t  checksum;
    char     oemId[6];
    uint8_t  revision;
    uint32_t rsdtAddress;
    uint32_t length;
    uint64_t xsdtAddress;
    uint8_t  extendedChecksum;
    uint8_t  reserved[3];
} __attribute__((packed));

// ACPI table header
struct AcpiTableHeader {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oemId[6];
    char oemTableId[8];
    uint32_t oemRevision;
    uint32_t creatorId;
    uint32_t creatorRevision;
} __attribute__((packed));

// ACPI XSDT structure
struct AcpiXsdt {
    AcpiTableHeader header;
    uint64_t tablePointers[];  // Variable-length field of pointers
} __attribute__((packed));

struct AcpiFacp {
    AcpiTableHeader header;
    uint32_t firmwareCtrl;
    uint32_t dsdt;

    // Field used in ACPI 1.0; no longer used in ACPI 2.0+
    uint8_t  reserved;

    uint8_t  preferredPowerManagementProfile;
    uint16_t sciInterrupt;
    uint32_t smiCommandPort;
    uint8_t  acpiEnable;
    uint8_t  acpiDisable;
    uint8_t  s4BiosReq;
    uint8_t  pstateControl;
    uint32_t pm1aEventBlock;
    uint32_t pm1bEventBlock;
    uint32_t pm1aControlBlock;
    uint32_t pm1bControlBlock;
    uint32_t pm2ControlBlock;
    uint32_t pmTimerBlock;
    uint32_t gpe0Block;
    uint32_t gpe1Block;
    uint8_t  pm1EventLength;
    uint8_t  pm1ControlLength;
    uint8_t  pm2ControlLength;
    uint8_t  pmTimerLength;
    uint8_t  gpe0Length;
    uint8_t  gpe1Length;
    uint8_t  gpe1Base;
    uint8_t  cstateControl;
    uint16_t worstC2Latency;
    uint16_t worstC3Latency;
    uint16_t flushSize;
    uint16_t flushStride;
    uint8_t  dutyOffset;
    uint8_t  dutyWidth;
    uint8_t  dayAlarm;
    uint8_t  monthAlarm;
    uint8_t  century;

    // Reserved in ACPI 1.0; used in ACPI 2.0+
    uint16_t bootArchitectureFlags;

    uint8_t  reserved2;
    uint32_t flags;

    // 12 byte structure; see GAS in ACPI specification
    GenericAddressStructure resetReg;

    uint8_t  resetValue;
    uint8_t  reserved3[3];

    // 64bit pointers - Available on ACPI 2.0+
    uint64_t x_firmwareControl;
    uint64_t x_dsdt;

    GenericAddressStructure x_pm1aEventBlock;
    GenericAddressStructure x_pm1bEventBlock;
    GenericAddressStructure x_pm1aControlBlock;
    GenericAddressStructure x_pm1bControlBlock;
    GenericAddressStructure x_pm2ControlBlock;
    GenericAddressStructure x_pmTimerBlock;
    GenericAddressStructure x_gpe0Block;
    GenericAddressStructure x_gpe1Block;
} __attribute__((packed));

class AcpiController {
public:
    __PRIVILEGED_CODE
    static AcpiController& get();

    __PRIVILEGED_CODE
    void init(void* rsdp);

    __PRIVILEGED_CODE
    inline uint64_t getAcpiTableEntryCount() const { return m_acpiTableEntries; }

private:
    AcpiXsdt*   m_xsdt;
    uint64_t    m_acpiTableEntries;
};

#endif
