#ifndef PCI_DEVICE_DRIVER_H
#define PCI_DEVICE_DRIVER_H
#include <modules/module_manager.h>
#include <pci/pci_device.h>
#include <interrupts/irq.h>

namespace drivers {
/**
 * @class pci_device_driver
 * @brief Base class for PCI device drivers.
 *
 * This abstract class defines the interface for PCI device drivers, providing
 * methods for probing and removing devices, as well as managing device-specific
 * resources and configuration.
 */
class pci_device_driver {
public:
    /**
     * @brief Constructs a PCI device driver with a given name.
     * @param name The name of the PCI device driver.
     */
    explicit pci_device_driver(const kstl::string& name)
        : m_name(name) {}

    /**
     * @brief Virtual destructor for the PCI device driver.
     */
    virtual ~pci_device_driver() = default;

    /**
     * @brief Attaches a PCI device to the driver.
     * @param dev Shared pointer to the PCI device to attach.
     * @param enable_bus_mastering Flag to enable or disable bus mastering for the device.
     * 
     * Associates the driver with a PCI device and configures the device as needed.
     */
    void attach_device(
        kstl::shared_ptr<pci::pci_device>& dev,
        bool enable_bus_mastering
    );

    /**
     * @brief Returns the name of the driver
     */
    inline const kstl::string& get_name() const { return m_name; }

    /**
     * @brief Initializes the attached PCI device.
     * @return True if the device is successfully initialized, false otherwise.
     *
     * Derived classes must implement this function to perform device-specific initialization.
     */
    virtual bool init_device() = 0;

    /**
     * @brief Starts the attached PCI device.
     * @return True if the device is successfully started, false otherwise.
     *
     * Derived classes must implement this function to activate or enable the device.
     */
    virtual bool start_device() = 0;

    /**
     * @brief Shuts down the attached PCI device.
     * @return True if the device is successfully shut down, false otherwise.
     *
     * Derived classes must implement this function to properly disable and clean up the device.
     */
    virtual bool shutdown_device() = 0;

protected:
    kstl::string m_name;                           // The name of the PCI device driver
    kstl::shared_ptr<pci::pci_device> m_pci_dev;   // Shared pointer to the associated PCI device
    uint8_t m_irq_vector = 0;                      // IRQ vector assigned to the PCI device
};
} // namespace drivers

#endif // PCI_DEVICE_DRIVER_H
