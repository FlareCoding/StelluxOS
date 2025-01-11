#include <pci/pci_capabilities.h>
#include <pci/pci.h>

namespace pci {
/**
 * @brief Converts a raw capability ID byte into our enum class.
 * 
 * @param raw_id The raw capability ID read from the PCI config space.
 * @return The corresponding capability_id enum value, or `capability_id::unknown` if not recognized.
 */
static capability_id to_capability_id(uint8_t raw_id) {
    switch (raw_id) {
    case 0x01: return capability_id::power_management;
    case 0x02: return capability_id::agp;
    case 0x03: return capability_id::vpd;
    case 0x04: return capability_id::slot_identification;
    case 0x05: return capability_id::msi;
    case 0x06: return capability_id::compact_pci_hotplug;
    case 0x07: return capability_id::pcix;
    case 0x08: return capability_id::hyper_transport;
    case 0x09: return capability_id::vendor_specific;
    case 0x0A: return capability_id::debug_port;
    case 0x0B: return capability_id::compact_pci_central;
    case 0x0C: return capability_id::pci_hotplug;
    case 0x0D: return capability_id::bridge_subsys_vid;
    case 0x0E: return capability_id::agp8x;
    case 0x0F: return capability_id::secure_device;
    case 0x10: return capability_id::pci_express;
    case 0x11: return capability_id::msi_x;
    case 0x12: return capability_id::sata_config;
    case 0x13: return capability_id::pci_advanced_features;
    default:   return capability_id::unknown;
    }
}

/**
 * @brief Enumerates all capabilities for a given device/function.
 * 
 * Uses the capability pointer (offset 0x34) in the PCI config header 
 * to walk the linked list of capabilities.
 * 
 * @param bus The bus number.
 * @param device The device number.
 * @param function The function number.
 * @return A vector of `pci_capability` structs containing all capabilities.
 */
__PRIVILEGED_CODE
void enumerate_capabilities(uint8_t bus, uint8_t device, uint8_t function, kstl::vector<pci_capability>& caps) {
    // Read status register to see if capabilities are available
    const uint16_t status = config::read_word(bus, device, function, PCI_STATUS_OFFSET);
    const bool has_capabilities = (status & (1 << 4)) != 0; // bit 4 of status = Capabilities List

    if (!has_capabilities) {
        // The device does not support a capabilities list
        return;
    }

    // Read the pointer to the first capability
    const uint8_t cap_ptr = config::read_byte(bus, device, function, 0x34);
    if (cap_ptr < 0x40) {
        // In practice, a pointer less than 0x40 is unusual or indicates no real capabilities
        return;
    }

    // We will walk the capabilities list until the next pointer is 0.
    uint8_t current_offset = cap_ptr;
    // To avoid infinite loops in case of malformed data, set a maximum iteration limit.
    constexpr int MAX_CAP_COUNT = 48; // typical arbitrary safe-guard

    for (int count = 0; count < MAX_CAP_COUNT && current_offset != 0; ++count) {
        // The first byte at the capability offset is the capability ID
        const uint8_t raw_id = config::read_byte(bus, device, function, current_offset);
        // The second byte is the pointer to the next capability
        const uint8_t next_cap_ptr = config::read_byte(bus, device, function, current_offset + 1);

        pci_capability cap;
        cap.id = to_capability_id(raw_id);
        cap.offset = current_offset;

        // The capability header is at least 2 bytes (ID and NEXT). 
        // Many capabilities have longer structures, but the length is not 
        // always at offset 2 for every capability type. 
        // Some use the 3rd byte or further to store their length.
        //
        // A robust approach is: read the capability-specific length if it exists,
        // or define a safe maximum, we read up to 32 bytes.
        //
        // A more advanced approach might parse well-known capabilities by ID and length. 
        // For now, we just store a fixed range or minimal subset.
        constexpr uint8_t CAP_READ_SIZE = 32; 
        cap.data.reserve(CAP_READ_SIZE);
        for (uint8_t i = 0; i < CAP_READ_SIZE; ++i) {
            // Safely read in from config space
            uint8_t val = config::read_byte(bus, device, function, current_offset + i);
            cap.data.push_back(val);
        }

        caps.push_back(cap);

        // Move to the next capability
        current_offset = next_cap_ptr;
    }
}

} // namespace pci
