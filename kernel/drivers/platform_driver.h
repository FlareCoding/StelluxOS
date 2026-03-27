#ifndef STELLUX_DRIVERS_PLATFORM_DRIVER_H
#define STELLUX_DRIVERS_PLATFORM_DRIVER_H

#include "drivers/device_driver.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"

namespace drivers {

/**
 * Base class for platform (non-PCI) device drivers.
 *
 * Platform devices are discovered via FDT or hardcoded addresses rather
 * than PCI bus enumeration. They use wired GIC SPI interrupts instead
 * of MSI/MSI-X. Examples: SoC-integrated Ethernet (BCM GENET),
 * on-chip peripherals, etc.
 *
 * Concrete platform drivers inherit from this class and implement
 * attach() and run().
 */
class platform_driver : public device_driver {
public:
    platform_driver(const char* name, uint64_t reg_phys, uint64_t reg_size,
                    uint32_t irq0 = 0, uint32_t irq1 = 0)
        : device_driver(name)
        , m_reg_phys(reg_phys)
        , m_reg_size(reg_size)
        , m_reg_va(0)
        , m_reg_map_base(0)
        , m_event_pending(false) {
        m_irq[0] = irq0;
        m_irq[1] = irq1;
        m_irq_wq.init();
        m_irq_lock = sync::SPINLOCK_INIT;
    }

    int32_t attach() override = 0;
    void run() override = 0;

protected:
    /**
     * Map the device register region into kernel virtual address space.
     * @return Mapped virtual address, or 0 on failure.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE uintptr_t map_regs();

    /**
     * Block the driver task until an interrupt or other event wakes it.
     * Multiple interrupts may coalesce into a single wake.
     */
    void wait_for_event();

    /**
     * Signal that an event is pending (called from ISR context).
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void notify_event();

    uint64_t         m_reg_phys;
    uint64_t         m_reg_size;
    uintptr_t        m_reg_va;
    uintptr_t        m_reg_map_base;
    uint32_t         m_irq[2];
    sync::wait_queue m_irq_wq;
    sync::spinlock   m_irq_lock;
    bool             m_event_pending;
};

/**
 * Initialize platform device drivers.
 * Probes FDT (and hardcoded addresses) for known platform devices,
 * instantiates matching drivers, calls attach(), and spawns kernel tasks.
 * Called after PCI driver init.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t platform_init();

} // namespace drivers

#endif // STELLUX_DRIVERS_PLATFORM_DRIVER_H
