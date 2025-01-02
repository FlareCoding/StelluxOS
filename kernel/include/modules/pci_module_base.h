#ifndef PCI_MODULE_BASE_H
#define PCI_MODULE_BASE_H
#include <modules/module_manager.h>
#include <pci/pci_device.h>

namespace modules {
/**
 * @class pci_module_base
 * @brief Base class for PCI-based modules.
 * 
 * Extends the `module_base` class to include PCI-specific functionality, enabling modules
 * to interact with PCI devices and handle their lifecycle.
 */
class pci_module_base : public module_base {
public:
    /**
     * @brief Constructs a PCI-based module with a given name.
     * @param name The name of the PCI module.
     * 
     * Initializes the module in the `unloaded` state and prepares it for PCI device attachment.
     */
    explicit pci_module_base(const kstl::string& name);

    /**
     * @brief Virtual destructor for the PCI module base class.
     * 
     * Ensures proper cleanup in derived classes.
     */
    virtual ~pci_module_base() = default;

    // ------------------------------------------------------------------------
    // Lifecycle Hooks (overrides from module_base)
    // ------------------------------------------------------------------------
    
    /**
     * @brief Initializes the PCI module.
     * @return True if the initialization succeeds, false otherwise.
     * 
     * This function must be implemented by derived classes and is called to prepare the module
     * for operation, such as initializing hardware or configuring the PCI device.
     */
    bool init() override = 0;

    /**
     * @brief Starts the PCI module.
     * @return True if the module starts successfully, false otherwise.
     * 
     * Transitions the module to the `running` state. Derived classes must implement the specific
     * logic for starting the module, such as enabling device interrupts.
     */
    bool start() override = 0;

    /**
     * @brief Stops the PCI module.
     * @return True if the module stops successfully, false otherwise.
     * 
     * Transitions the module to the `stopped` state. Derived classes must implement the specific
     * logic for stopping the module, such as disabling device functionality.
     */
    bool stop() override = 0;

protected:
    kstl::shared_ptr<pci::pci_device> m_pci_dev; /** Shared pointer to the associated PCI device class  */
    uint8_t m_irq_vector = 0;                    /** IRQ vector assigned to the PCI device */

private:
    friend class module_manager;

    /**
     * @brief Attaches a PCI device to the module.
     * @param dev Shared pointer to the PCI device to attach.
     * @param enable_bus_mastering Flag to enable or disable bus mastering for the device.
     * 
     * Associates the module with a PCI device and configures the device as needed. This function
     * is called by the `module_manager` during module initialization or discovery.
     */
    void attach_device(
        kstl::shared_ptr<pci::pci_device>& dev,
        bool enable_bus_mastering
    );
};
} // namespace modules

#endif // PCI_MODULE_BASE_H
