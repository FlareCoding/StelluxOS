#ifndef PCI_MODULE_BASE_H
#define PCI_MODULE_BASE_H
#include <modules/module_manager.h>
#include <pci/pci_device.h>

namespace modules {
class pci_module_base : public module_base {
public:
    explicit pci_module_base(
        const kstl::string& name
    );

    virtual ~pci_module_base() = default;

    // ------------------------------------------------------------------------
    // Lifecycle Hooks (overrides from module_base)
    // ------------------------------------------------------------------------
    
    bool init() override = 0;
    bool start() override = 0;
    bool stop() override = 0;

protected:
    kstl::shared_ptr<pci::pci_device> m_pci_dev;
    uint8_t m_irq_vector = 0;

private:
    friend class module_manager;

    void attach_device(
        kstl::shared_ptr<pci::pci_device>& dev,
        bool enable_bus_mastering
    );
};

} // namespace modules

#endif // PCI_MODULE_BASE_H
