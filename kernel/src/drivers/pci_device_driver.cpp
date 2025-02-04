#include <drivers/pci_device_driver.h>
#include <interrupts/irq.h>
#include <dynpriv/dynpriv.h>
#include <serial/serial.h>

#ifdef ARCH_X86_64
#include <arch/x86/cpuid.h>
#endif

namespace drivers {
void pci_device_driver::attach_device(
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

        bool m_qemu_detected = false;

#ifdef ARCH_X86_64
        m_qemu_detected = arch::x86::cpuid_is_running_under_qemu();
#endif

        // Check if the device requires an IRQ vector
        uint8_t legacy_irq_line = dev->legacy_irq_line();

        if (dev->has_capability(pci::capability_id::msi) && !m_qemu_detected) {
            // Allocate a free IRQ vector
            m_irq_vector = find_free_irq_vector();

            if (m_irq_vector) {
                // Ensure that the found IRQ vector is marked as reserved
                reserve_irq_vector(m_irq_vector);

                // Setup MSI
                dev->setup_msi(0, m_irq_vector);
            }
        } else if (dev->has_capability(pci::capability_id::msi_x) && !m_qemu_detected) {
            // Allocate a free IRQ vector
            m_irq_vector = find_free_irq_vector();

            if (m_irq_vector) {
                // Ensure that the found IRQ vector is marked as reserved
                reserve_irq_vector(m_irq_vector);

                // Setup MSI-X
                dev->setup_msix(0, m_irq_vector);
            }
        } else if (legacy_irq_line != 255 && legacy_irq_line != 0) {
            // Allocate a free IRQ vector
            m_irq_vector = find_free_irq_vector();

            if (m_irq_vector) {
                // Ensure that the found IRQ vector is marked as reserved
                reserve_irq_vector(m_irq_vector);

                // Route the legacy IRQ line to the allocated IRQ vector
                route_legacy_irq(legacy_irq_line, m_irq_vector, 0, IRQ_LEVEL_TRIGGERED);
            }
        }
    });
}
} // namespace drivers
