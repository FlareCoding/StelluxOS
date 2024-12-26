#include <modules/pci_module_base.h>
#include <dynpriv/dynpriv.h>

namespace modules {
pci_module_base::pci_module_base(
    const kstl::string& name
) : module_base(name) {}

void pci_module_base::attach_device(
    kstl::shared_ptr<pci::pci_device>& dev,
    bool enable_bus_mastering
) {
    m_pci_dev = dev;

    RUN_ELEVATED({
        dev->enable();

        if (enable_bus_mastering) {
            dev->enable_bus_mastering();
        }
    });
}
} // namespace modules
