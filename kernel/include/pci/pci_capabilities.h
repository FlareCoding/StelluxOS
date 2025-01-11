#ifndef PCI_CAPABILITIES_H
#define PCI_CAPABILITIES_H
#include <kstl/vector.h>

namespace pci {

/**
 * @enum capability_id
 * @brief Enumerates known PCI capability IDs (common subset).
 * 
 * Feel free to expand this list with additional 
 * capability IDs relevant to your needs.
 */
enum class capability_id : uint8_t {
    power_management   = 0x01,
    agp                = 0x02,
    vpd                = 0x03,
    slot_identification= 0x04,
    msi                = 0x05,
    compact_pci_hotplug= 0x06,
    pcix               = 0x07,
    hyper_transport    = 0x08,
    vendor_specific    = 0x09,
    debug_port         = 0x0A,
    compact_pci_central= 0x0B,
    pci_hotplug        = 0x0C,
    bridge_subsys_vid  = 0x0D,
    agp8x              = 0x0E,
    secure_device      = 0x0F,
    pci_express        = 0x10,
    msi_x              = 0x11,
    sata_config        = 0x12,
    pci_advanced_features = 0x13,

    unknown            = 0xFF
};

/**
 * @struct pci_capability
 * @brief Generic representation of a PCI capability.
 * 
 * This struct stores the capability ID, the offset in the
 * configuration space, and the raw data for further parsing.
 */
struct pci_capability {
    capability_id id;            // Capability ID (e.g., MSI, PCIe, etc.)
    uint8_t       offset;        // Offset in the PCI config space
    kstl::vector<uint8_t> data;  // Raw bytes for this capability
};

/**
 * @brief Enumerates all capabilities for a given device/function.
 * 
 * Uses the capability pointer (offset 0x34) in the PCI config header 
 * to walk the linked list of capabilities.
 * 
 * @param bus The bus number.
 * @param device The device number.
 * @param function The function number.
 * @param caps A reference to a vector of `pci_capability` structs to populate.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void enumerate_capabilities(uint8_t bus, uint8_t device, uint8_t function, kstl::vector<pci_capability>& caps);
} // namespace pci

#endif // PCI_CAPABILITIES_H
