#ifndef RSDT_H
#define RSDT_H
#include <ktypes.h>

// ACPI RSDP (Root System Description Pointer)
struct ACPIRsdp {
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
struct ACPIXsdt {
    AcpiTableHeader header;
    uint64_t tablePointers[];  // Variable-length field of pointers
} __attribute__((packed));

// MADT structure
struct Madt {
    AcpiTableHeader header;
    uint32_t localApicAddress;
    uint32_t flags;
    uint8_t tableEntries[];  // Variable-length
} __attribute__((packed));

// Local APIC entry in MADT
struct MadtLocalApic {
    uint8_t type; // should be 0 for Local APIC
    uint8_t length; // should be 8 for Local APIC
    uint8_t acpiProcessorId;
    uint8_t apicId;
    uint32_t flags;
} __attribute__((packed));

uint32_t queryCpuCount(Madt* madt);

#endif
