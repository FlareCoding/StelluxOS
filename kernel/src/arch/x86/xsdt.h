#ifndef RSDT_H
#define RSDT_H
#include <ktypes.h>

// ACPI RSDP (Root System Description Pointer)
typedef struct {
    char     Signature[8];
    uint8_t  Checksum;
    char     OemId[6];
    uint8_t  Revision;
    uint32_t RsdtAddress;
    uint32_t Length;
    uint64_t XsdtAddress;
    uint8_t  ExtendedChecksum;
    uint8_t  Reserved[3];
} __attribute__((packed)) rsdp_t;

// ACPI table header
typedef struct {
    char Signature[4];
    uint32_t Length;
    uint8_t Revision;
    uint8_t Checksum;
    char OemId[6];
    char OemTableId[8];
    uint32_t OemRevision;
    uint32_t CreatorId;
    uint32_t CreatorRevision;
} __attribute__((packed)) AcpiTableHeader;

// XSDT structure
typedef struct {
    AcpiTableHeader Header;
    uint64_t TablePointers[];  // Variable-length field of pointers
} __attribute__((packed)) Xsdt;

// MADT structure
typedef struct {
    AcpiTableHeader Header;
    uint32_t LocalApicAddress;
    uint32_t Flags;
    uint8_t TableEntries[];  // Variable-length
} __attribute__((packed)) Madt;

// Local APIC entry in MADT
typedef struct {
    uint8_t Type; // should be 0 for Local APIC
    uint8_t Length; // should be 8 for Local APIC
    uint8_t AcpiProcessorId;
    uint8_t ApicId;
    uint32_t Flags;
} __attribute__((packed)) MadtLocalApic;

uint32_t get_cpu_count(Madt* madt);

#endif
