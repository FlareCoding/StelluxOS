#include <modules/pci_module_base.h>
#include <interrupts/irq.h>
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
        // Enable the IO/MMIO accesses to the device
        dev->enable();

        // Enable bus mastering if needed
        if (enable_bus_mastering) {
            dev->enable_bus_mastering();
        }

        // Check if the device requires an IRQ vector
        uint8_t legacy_irq_line = dev->legacy_irq_line();
        if (legacy_irq_line != 255 && legacy_irq_line != 0) {
            // Allocate a free IRQ vector
            m_irq_vector = find_free_irq_vector();

            // Route the legacy IRQ line to the allocated IRQ vector
            route_legacy_irq(legacy_irq_line, m_irq_vector, 0, IRQ_LEVEL_TRIGGERED);
        }
    });
}
} // namespace modules
