#ifndef MADT_H
#define MADT_H
#include "acpi.h"
#include <kvector.h>
#include <arch/x86/ioapic.h>

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

struct IoApicDescriptor {
    uint8_t type;   // should be 1 for IOAPIC
    uint8_t length; // should be 12 for IOAPIC
    uint8_t ioapicId;
    uint8_t reserved;
    uint32_t ioapicAddress; // This is the base address of the IOAPIC
    uint32_t globalSystemInterruptBase;
} __attribute__((packed));

class Madt {
public:
    Madt(MadtDescriptor* desc);
    ~Madt() = default;

    LocalApicDescriptor& getLocalApicDescriptor(size_t idx) { return m_localApics[idx]; }
    kstl::SharedPtr<IoApic>& getIoApic(size_t idx) { return m_ioApics[idx]; }

    size_t getCpuCount() const { return m_localApics.size(); }
    size_t getIoApicCount() const { return m_ioApics.size(); }

private:
    kstl::vector<LocalApicDescriptor> m_localApics;
    kstl::vector<kstl::SharedPtr<IoApic>> m_ioApics;
};

#endif
