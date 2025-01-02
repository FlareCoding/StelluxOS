#ifndef MADT_H
#define MADT_H
#include "acpi.h"
#include <memory/memory.h>
#include <kstl/vector.h>

#define MADT_DESCRIPTOR_TYPE_LAPIC                      0
#define MADT_DESCRIPTOR_TYPE_IOAPIC                     1
#define MADT_DESCRIPTOR_TYPE_IOAPIC_IRQ_SRC_OVERRIDE    2
#define MADT_DESCRIPTOR_TYPE_IOAPIC_NMI_SOURCE          3
#define MADT_DESCRIPTOR_TYPE_LAPIC_NMI                  4
#define MADT_DESCRIPTOR_TYPE_LAPIC_ADDRESS_OVERRIDE     5
#define MADT_DESCRIPTOR_TYPE_PROCESSOR_LOCAL_X2APIC     9

//
// If flags bit 0 is set the CPU is able to be enabled,
// if it is not set you need to check bit 1. If that one
// is set, you can still enable it, but if it is not, the
// CPU can not be enabled and the OS should not try.
//
#define LAPIC_PROCESSOR_ENABLED_BIT         (1 << 0)
#define LAPIC_PROCESSOR_ONLINE_CAPABLE_BIT  (1 << 1)

namespace acpi {
struct madt_table {
    acpi_sdt_header header;
    uint32_t        lapic_address;
    uint32_t        flags;
    uint8_t         entries[];
} __attribute__((packed));

struct lapic_desc {
    uint8_t type;
    uint8_t length;
    uint8_t acpi_processor_id;
    uint8_t apic_id;
    uint32_t flags;
} __attribute__((packed));

struct ioapic_desc {
    uint8_t type;
    uint8_t length;
    uint8_t ioapic_id;
    uint8_t reserved;
    uint32_t ioapic_address; // This is the base address of the IOAPIC
    uint32_t global_system_interrupt_base;
} __attribute__((packed));

struct ioapic_irq_source_override_desc {
    uint8_t type;
    uint8_t length;
    uint8_t bus_source;
    uint8_t irq_source;
    uint32_t gsi;   // Global System Interrupt
    uint16_t flags;
} __attribute__((packed));

struct ioapic_nmi_source_desc {
    uint8_t type;
    uint8_t length;
    uint8_t nmi_source;
    uint8_t rsvd;
    uint16_t flags;
    uint32_t gsi;   // Global System Interrupt
} __attribute__((packed));

struct lapic_nmi_desc {
    uint8_t type;
    uint8_t length;
    uint8_t apic_processor_id; // (0xFF means all processors)
    uint16_t flags;
    uint8_t lint; // LINT# (0 or 1)
} __attribute__((packed));

struct lapic_address_override_desc {
    uint8_t type;
    uint8_t length;
    uint16_t rsvd;
    uint64_t address;
} __attribute__((packed));

struct lapic_x2apic_desc {
    uint8_t type;
    uint8_t length;
    uint16_t rsvd;
    uint32_t x2apic_id; // Processor's local x2APIC ID
    uint32_t flags;     // Same as the Local APIC flags
    uint32_t acpi_id;
} __attribute__((packed));

/**
 * @class madt
 * @brief Manages the MADT (Multiple APIC Description Table) and provides access to LAPICs.
 * 
 * This class is responsible for parsing and storing the LAPIC entries from the MADT.
 * It provides methods to retrieve information about the local APICs and the number of CPUs.
 */
class madt {
public:
    /**
     * @brief Gets the singleton instance of the madt class.
     * @return The singleton madt instance.
     * 
     * This function ensures that only one instance of the madt class is used, providing a global point of access.
     */
    static madt& get();

    /**
     * @brief Default constructor for the madt class.
     */
    madt() = default;

    /**
     * @brief Default destructor for the madt class.
     */
    ~madt() = default;

    /**
     * @brief Initializes the madt object using the provided ACPI MADT table.
     * @param acpi_madt_table Pointer to the ACPI MADT table header.
     * 
     * This function processes the provided MADT table and extracts information related to LAPICs.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void init(acpi_sdt_header* acpi_madt_table);

    /**
     * @brief Returns a reference to the vector of LAPIC descriptors.
     * @return Reference to the vector containing LAPIC descriptors.
     * 
     * This function allows access to the LAPIC descriptors, which are part of the parsed MADT table.
     */
    kstl::vector<lapic_desc>& get_lapics() { return m_local_apics; }

    /**
     * @brief Returns the number of CPUs (based on LAPIC entries).
     * @return The number of LAPIC entries (i.e., CPUs).
     * 
     * This function computes the CPU count by checking the number of LAPIC entries in the MADT.
     */
    __force_inline__ size_t get_cpu_count() const { return m_local_apics.size(); }

private:
    kstl::vector<lapic_desc> m_local_apics;
};
} // namespace acpi
#endif // MADT_H
