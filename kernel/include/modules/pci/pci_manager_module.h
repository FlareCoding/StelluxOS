#ifndef PCI_MANAGER_MODULE_H
#define PCI_MANAGER_MODULE_H
#include <modules/module_manager.h>
#include <pci/pci_device.h>
#include <drivers/pci_device_driver.h>

namespace modules {
class pci_manager_module : public module_base {
public:
    pci_manager_module();
    ~pci_manager_module() = default;

    // ------------------------------------------------------------------------
    // Lifecycle Hooks (overrides from module_base)
    // ------------------------------------------------------------------------

    bool init() override;
    bool start() override;
    bool stop() override;

    bool on_command(
        uint64_t  command,
        const void* data_in,
        size_t      data_in_size,
        void*       data_out,
        size_t      data_out_size
    ) override;

private:
    static void _driver_thread_entry(drivers::pci_device_driver* driver);
};
} // namespace modules

#endif // PCI_MODULE_BASE_H
