#ifndef PCI_MANAGER_H
#define PCI_MANAGER_H
#include "pci_device.h"
#include <acpi/acpi.h>

namespace pci {
struct pci_segment_entry {
    uint64_t base_address;      // Base address for this PCI segment
    uint16_t pci_segment_group; // Segment group number
    uint8_t start_bus;          // Starting bus number
    uint8_t end_bus;            // Ending bus number
    uint32_t reserved;          // Reserved, must be 0
} __attribute__((packed));

struct mcfg_table {
    acpi::acpi_sdt_header header;   // Standard ACPI table header
    uint64_t reserved;              // Reserved field
} __attribute__((packed));

class pci_manager {
public:
    static pci_manager& get();

    pci_manager() = default;
    ~pci_manager() = default;

    void init(acpi::acpi_sdt_header* mcfg_table);

    const kstl::vector<kstl::shared_ptr<pci_device>>& get_devices() const {
        return m_devices;
    }

    // Find the first device that matches the given vendor_id and device_id.
    // Returns nullptr if not found.
    kstl::shared_ptr<pci_device> find_device(uint16_t vendor_id, uint16_t device_id) const;

    // Find all devices that match the given vendor_id and device_id.
    kstl::vector<kstl::shared_ptr<pci_device>> find_all_devices(uint16_t vendor_id, uint16_t device_id) const;

    // Find the first device that matches the given class_code and subclass.
    // Returns nullptr if not found.
    kstl::shared_ptr<pci_device> find_by_class(uint8_t class_code, uint8_t subclass) const;

    // Find all devices that match the given class_code and subclass.
    kstl::vector<kstl::shared_ptr<pci_device>> find_all_by_class(uint8_t class_code, uint8_t subclass) const;

private:
    kstl::vector<kstl::shared_ptr<pci_device>> m_devices;
    pci_segment_entry* m_mcfg_segment;

    void _parse_mcfg(acpi::acpi_sdt_header* mcfg_ptr);
    void _enumerate_bus(uint64_t segment_base_addr, uint8_t bus);
    void _enumerate_device(uint64_t bus_addr, uint8_t device);
    void _enumerate_function(uint64_t device_addr, uint8_t function);
};
} // namespace pci

#endif // PCI_MANAGER_H
