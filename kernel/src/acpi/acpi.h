#ifndef ACPI_H
#define ACPI_H
#include <ktypes.h>

// Generic Address Structure (GAS) as defined in the ACPI specification
struct GenericAddressStructure {
    uint8_t  addressSpace;
    uint8_t  bitWidth;
    uint8_t  bitOffset;
    uint8_t  accessSize;
    uint64_t address;
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

#endif
