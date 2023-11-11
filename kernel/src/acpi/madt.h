#ifndef MADT_H
#define MADT_H
#include "acpi.h"
#include <kvector.h>

// MADT structure
struct MadtDescriptor {
    AcpiTableHeader header;
    uint32_t localApicAddress;
    uint32_t flags;
    uint8_t tableEntries[];
} __attribute__((packed));

// Local APIC entry in MADT
struct LocalApicDescriptor {
    uint8_t type;   // should be 0 for Local APIC
    uint8_t length; // should be 8 for Local APIC
    uint8_t acpiProcessorId;
    uint8_t apicId;
    uint32_t flags;
} __attribute__((packed));

class Madt {
public:
    Madt(MadtDescriptor* desc);
    ~Madt() = default;

private:
    kstl::vector<LocalApicDescriptor> m_localApics;
};

#endif
