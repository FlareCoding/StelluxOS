#ifndef PCI_MANAGER_H
#define PCI_MANAGER_H
#include "pci_device.h"
#include <acpi/acpi.h>

namespace pci {
/**
 * @struct pci_segment_entry
 * @brief Represents a PCI segment entry in the MCFG (Memory-Mapped Configuration Space) table.
 * 
 * Describes the memory-mapped configuration space for a specific PCI segment group.
 */
struct pci_segment_entry {
    uint64_t base_address;      // Base address for this PCI segment
    uint16_t pci_segment_group; // Segment group number
    uint8_t start_bus;          // Starting bus number
    uint8_t end_bus;            // Ending bus number
    uint32_t reserved;          // Reserved, must be 0
} __attribute__((packed));

/**
 * @struct mcfg_table
 * @brief Represents the ACPI MCFG (Memory-Mapped Configuration Space) table.
 * 
 * Contains information about PCI segments and their memory-mapped configuration space.
 */
struct mcfg_table {
    acpi::acpi_sdt_header header;   // Standard ACPI table header
    uint64_t reserved;              // Reserved field
} __attribute__((packed));

/**
 * @class pci_manager
 * @brief Manages and enumerates PCI devices in the system.
 * 
 * Provides functionality for initializing PCI devices, discovering their properties, and querying
 * for specific devices by their identifiers or class codes.
 */
class pci_manager {
public:
    /**
     * @brief Retrieves the singleton instance of the PCI manager.
     * @return Reference to the singleton instance of the `pci_manager`.
     */
    static pci_manager& get();

    /**
     * @brief Default constructor for the PCI manager.
     */
    pci_manager() = default;

    /**
     * @brief Default destructor for the PCI manager.
     */
    ~pci_manager() = default;

    /**
     * @brief Initializes the PCI manager using the MCFG table.
     * @param mcfg_table Pointer to the ACPI MCFG table.
     * 
     * Parses the MCFG table to identify PCI segments and enumerates all PCI devices.
     */
    void init(acpi::acpi_sdt_header* mcfg_table);

    /**
     * @brief Retrieves a list of all discovered PCI devices.
     * @return A constant reference to a vector of shared pointers to `pci_device` objects.
     */
    const kstl::vector<kstl::shared_ptr<pci_device>>& get_devices() const {
        return m_devices;
    }

    /**
     * @brief Finds the first PCI device that matches the given vendor ID and device ID.
     * @param vendor_id The vendor ID of the target device.
     * @param device_id The device ID of the target device.
     * @return Shared pointer to the matching `pci_device`, or `nullptr` if not found.
     */
    kstl::shared_ptr<pci_device> find_device(uint16_t vendor_id, uint16_t device_id) const;

    /**
     * @brief Finds all PCI devices that match the given vendor ID and device ID.
     * @param vendor_id The vendor ID of the target devices.
     * @param device_id The device ID of the target devices.
     * @return A vector of shared pointers to the matching `pci_device` objects.
     */
    kstl::vector<kstl::shared_ptr<pci_device>> find_all_devices(uint16_t vendor_id, uint16_t device_id) const;

    /**
     * @brief Finds the first PCI device that matches the given class code and subclass.
     * @param class_code The class code of the target device.
     * @param subclass The subclass code of the target device.
     * @return Shared pointer to the matching `pci_device`, or `nullptr` if not found.
     */
    kstl::shared_ptr<pci_device> find_by_class(uint8_t class_code, uint8_t subclass) const;

    /**
     * @brief Finds the first PCI device that matches the given class code, subclass, and programming interface.
     * @param class_code The class code of the target device.
     * @param subclass The subclass code of the target device.
     * @param prog_if The programming interface of the target device.
     * @return Shared pointer to the matching `pci_device`, or `nullptr` if not found.
     */
    kstl::shared_ptr<pci_device> find_by_progif(uint8_t class_code, uint8_t subclass, uint8_t prog_if) const;

    /**
     * @brief Finds all PCI devices that match the given class code and subclass.
     * @param class_code The class code of the target devices.
     * @param subclass The subclass code of the target devices.
     * @return A vector of shared pointers to the matching `pci_device` objects.
     */
    kstl::vector<kstl::shared_ptr<pci_device>> find_all_by_class(uint8_t class_code, uint8_t subclass) const;

private:
    kstl::vector<kstl::shared_ptr<pci_device>> m_devices; /** List of all discovered PCI devices */
    pci_segment_entry* m_mcfg_segment;                    /** Pointer to the MCFG segment entries */

    /**
     * @brief Parses the MCFG table to extract PCI segment entries.
     * @param mcfg_ptr Pointer to the ACPI MCFG table.
     */
    void _parse_mcfg(acpi::acpi_sdt_header* mcfg_ptr);

    /**
     * @brief Enumerates all devices on a specific PCI bus.
     * @param segment_base_addr Base address of the PCI segment.
     * @param bus Bus number to enumerate.
     */
    void _enumerate_bus(uint64_t segment_base_addr, uint8_t bus);

    /**
     * @brief Enumerates all functions on a specific PCI device.
     * @param bus_addr Address of the PCI bus containing the device.
     * @param device Device number to enumerate.
     */
    void _enumerate_device(uint64_t bus_addr, uint8_t device);

    /**
     * @brief Enumerates a specific PCI function on a device.
     * @param device_addr Address of the PCI device containing the function.
     * @param function Function number to enumerate.
     */
    void _enumerate_function(uint64_t device_addr, uint8_t function);
};
} // namespace pci

#endif // PCI_MANAGER_H
