#ifndef PCI_DEVICE_H
#define PCI_DEVICE_H
#include "pci.h"
#include "pci_capabilities.h"

namespace pci {
/**
 * @class pci_device
 * @brief Represents a PCI device and provides methods to manage and interact with it.
 * 
 * Encapsulates information about a PCI device, including its configuration space, BARs (Base Address Registers),
 * and control operations such as enabling/disabling the device and bus mastering.
 */
class pci_device {
public:
    /**
     * @brief Constructs a PCI device instance.
     * @param function_address The physical address of the PCI function.
     * @param desc Pointer to a `pci_function_desc` structure containing the device description.
     * 
     * Initializes the PCI device with its configuration and address details.
     */
    pci_device(uint64_t function_address, pci_function_desc* desc);

    /**
     * @brief Retrieves the vendor ID of the PCI device.
     * @return The 16-bit vendor ID.
     */
    uint16_t vendor_id() const { return m_desc->vendor_id; }

    /**
     * @brief Retrieves the device ID of the PCI device.
     * @return The 16-bit device ID.
     */
    uint16_t device_id() const { return m_desc->device_id; }

    /**
     * @brief Retrieves the class code of the PCI device.
     * @return The 8-bit class code.
     */
    uint8_t class_code() const { return m_desc->class_code; }

    /**
     * @brief Retrieves the subclass code of the PCI device.
     * @return The 8-bit subclass code.
     */
    uint8_t subclass() const { return m_desc->subclass; }

    /**
     * @brief Retrieves the programming interface (prog IF) of the PCI device.
     * @return The 8-bit programming interface value.
     */
    uint8_t prog_if() const { return m_desc->prog_if; }

    /**
     * @brief Retrieves the revision ID of the PCI device.
     * @return The 8-bit revision ID.
     */
    uint8_t revision_id() const { return m_desc->revision_id; }

    /**
     * @brief Retrieves the legacy IRQ line assigned to the PCI device.
     * @return The 8-bit legacy IRQ line value.
     */
    uint8_t legacy_irq_line() const { return m_desc->interrupt_line; }

    /**
     * @brief Enables the PCI device.
     * 
     * Sets the memory space and IO space bits in the command register, making the device operational.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void enable();

    /**
     * @brief Disables the PCI device.
     * 
     * Clears the memory and IO space bits in the command register, effectively disabling the device.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void disable();

    /**
     * @brief Enables bus mastering for the PCI device.
     * 
     * Sets the bus mastering bit in the command register, allowing the device to initiate DMA transactions.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void enable_bus_mastering();

    /**
     * @brief Retrieves the Base Address Registers (BARs) of the PCI device.
     * @return A reference to a vector of `pci_bar` objects representing the device's BARs.
     */
    const kstl::vector<pci_bar>& get_bars() const { return m_bars; }

    /**
     * @brief Retrieves the list capabilities of the PCI device.
     * @return A reference to a vector of `pci_capability` objects representing the device's PCI capabilities.
     */
    const kstl::vector<pci_capability>& get_capabilities() const { return m_caps; }

    /**
     * @brief Retrieves a pointer to a capability by its ID, or nullptr if not found.
     * @param cap_id The ID of the capability to find.
     * @return Pointer to the capability, or nullptr if not present.
     */
    const pci_capability* find_capability(capability_id cap_id) const;
    
    /**
     * @brief Checks whether a given PCI capability is present in the device.
     * @param cap_id The ID of the capability to find.
     * @return True if the capability was found, false otherwise.
     */
    inline bool has_capability(capability_id cap_id) const { return (find_capability(cap_id) != nullptr); }

    /**
     * @brief Configures and enables MSI for this device, routing IRQ to the specified CPU and vector.
     * @param cpu The destination CPU's APIC ID (in xAPIC mode).
     * @param vector The IDT vector you want this MSI to trigger.
     * @param edgetrigger Whether the IRQ is edge-triggered or level-triggered.
     * @param deassert Deassert value of the IRQ.
     * @return True if MSI configuration was successful, false otherwise.
     * 
     * @note This requires that the device actually supports an MSI capability.
     *       If the device doesn't have MSI or is already in MSI-X mode, this may fail.
     */
    __PRIVILEGED_CODE bool setup_msi(uint8_t cpu, uint8_t vector, uint8_t edgetrigger = 1, uint8_t deassert = 1);

    /**
     * @brief Configures and enables MSI-X for this device, routing IRQ to the specified CPU and vector.
     * @param cpu The destination CPU's APIC ID (in xAPIC mode).
     * @param vector The IDT vector you want this MSI-X interrupt to trigger.
     * @param edgetrigger Whether the IRQ is edge-triggered or level-triggered.
     * @param deassert Deassert value of the IRQ.
     * @return True if MSI-X configuration was successful, false otherwise.
     *
     * @note This requires that the device actually supports MSI-X capability.
     */
    __PRIVILEGED_CODE bool setup_msix(uint8_t cpu, uint8_t vector, uint8_t edgetrigger = 1, uint8_t deassert = 1);

    /**
     * @brief Prints debug information about the PCI device.
     * 
     * Outputs the device's information, including its BARs, for diagnostic purposes.
     */
    void dbg_print_to_string() const;

private:
    uint32_t m_function_address; /** Physical address of the PCI function */
    pci_function_desc* m_desc;   /** Pointer to the descriptor containing the device's configuration data */

    uint8_t m_bus;      /** Bus number of the PCI device */
    uint8_t m_device;   /** Device number of the PCI device */
    uint8_t m_function; /** Function number of the PCI device */

    kstl::vector<pci_bar> m_bars; /** Vector of Base Address Registers (BARs) for the device */
    kstl::vector<pci_capability> m_caps; /** Vector of PCI capabilities for the device */

    /**
     * @brief Parses and initializes the BARs for the PCI device.
     * 
     * Reads the configuration space to populate the list of BARs.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void _parse_bars();

    /**
     * @brief Parses and stores a list of PCI capabilities for the device.
     * 
     * Reads the configuration space to populate the list of PCI capabilities.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void _parse_capabilities();

    /**
     * @brief Writes a value to the command register.
     * @param value The 16-bit value to write to the command register.
     * 
     * Updates the command register to control device operations.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void _write_command_register(uint16_t value);

    /**
     * @brief Reads the current value of the command register.
     * @return The 16-bit value of the command register.
     * 
     * Retrieves the command register value for device status or control.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE uint16_t _read_command_register();

    /**
     * @brief Maps the MSI-X vector table or PBA into memory from the appropriate BAR register.
     * @return Base address of the mapped data structure.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void* _map_msix_table_or_pba(const pci_bar& bar, uint32_t offset, uint32_t size);
};
} // namespace pci

#endif // PCI_DEVICE_H
