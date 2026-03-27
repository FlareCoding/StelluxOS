#include "drivers/platform_driver.h"
#include "drivers/net/bcm_genet.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "sched/sched.h"
#include "fdt/fdt.h"
#include "common/logging.h"
#include "dynpriv/dynpriv.h"

namespace drivers {

// ============================================================================
// platform_driver base class implementation
// ============================================================================

__PRIVILEGED_CODE uintptr_t platform_driver::map_regs() {
    if (m_reg_va != 0) {
        return m_reg_va; // already mapped
    }

    // Map as user-accessible so the driver task (which runs at EL0/Ring 3)
    // can access MMIO registers directly without RUN_ELEVATED on every
    // register read/write. This matches how device MMIO is typically
    // handled — the memory region is not security-sensitive kernel data.
    int32_t rc = vmm::map_device(
        static_cast<pmm::phys_addr_t>(m_reg_phys),
        m_reg_size,
        paging::PAGE_READ | paging::PAGE_WRITE | paging::PAGE_USER,
        m_reg_map_base,
        m_reg_va);

    if (rc != vmm::OK) {
        log::error("platform: failed to map device at 0x%lx (%d)",
                   m_reg_phys, rc);
        return 0;
    }

    return m_reg_va;
}

void platform_driver::wait_for_event() {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_irq_lock);
        while (!m_event_pending) {
            irq = sync::wait(m_irq_wq, m_irq_lock, irq);
        }
        m_event_pending = false;
        sync::spin_unlock_irqrestore(m_irq_lock, irq);
    });
}

__PRIVILEGED_CODE void platform_driver::notify_event() {
    sync::irq_state irq = sync::spin_lock_irqsave(m_irq_lock);
    m_event_pending = true;
    sync::spin_unlock_irqrestore(m_irq_lock, irq);
    sync::wake_one(m_irq_wq);
}

// ============================================================================
// Platform device discovery and initialization
// ============================================================================

static void platform_task_entry(void* arg) {
    auto* drv = static_cast<platform_driver*>(arg);

    // Perform hardware attach in the driver's own kernel task, not on the
    // boot thread. This avoids blocking init/shell/DM startup during slow
    // hardware initialization (MDIO scan, PHY reset, DMA allocation).
    int32_t rc = drv->attach();
    if (rc != 0) {
        log::error("platform: %s attach failed (%d), task exiting", drv->name(), rc);
        return;
    }

    drv->run();
}

// Known RPi4 GENET addresses (used when FDT doesn't have them or ACPI is used)
constexpr uint64_t BCM2711_GENET_BASE = 0xfd580000;
constexpr uint64_t BCM2711_GENET_SIZE = 0x10000;
// GIC SPI 157 = IRQ 189, SPI 158 = IRQ 190
constexpr uint32_t BCM2711_GENET_IRQ0 = 189;
constexpr uint32_t BCM2711_GENET_IRQ1 = 190;

__PRIVILEGED_CODE static int32_t probe_genet() {
    uint64_t reg_phys = 0, reg_size = 0;
    uint32_t irqs[2] = { 0, 0 };

    // Try FDT first
    bool found = false;
    int32_t fdt_rc = fdt::init();
    if (fdt_rc == fdt::OK) {
        int32_t node = fdt::find_compatible("brcm,bcm2711-genet-v5");
        if (node >= 0) {
            found = true;
            fdt::get_reg(node, &reg_phys, &reg_size);

            int32_t nirqs = fdt::get_interrupts(node, irqs, 2);
            if (nirqs < 2) {
                irqs[0] = BCM2711_GENET_IRQ0;
                irqs[1] = BCM2711_GENET_IRQ1;
            }

            // Use known addresses as fallback if FDT reg was empty
            if (reg_phys == 0) {
                reg_phys = BCM2711_GENET_BASE;
                reg_size = BCM2711_GENET_SIZE;
            }

            log::info("platform: found GENET via FDT at 0x%lx size 0x%lx, IRQs %u,%u",
                      reg_phys, reg_size, irqs[0], irqs[1]);
        }
    }

    if (!found) {
        // ACPI-booted RPi4 (via UEFI) may not pass FDT to the kernel.
        // Fall back to the known BCM2711 GENET MMIO address.
        reg_phys = BCM2711_GENET_BASE;
        reg_size = BCM2711_GENET_SIZE;
        irqs[0] = BCM2711_GENET_IRQ0;
        irqs[1] = BCM2711_GENET_IRQ1;
        log::info("platform: GENET not in FDT, using known BCM2711 addresses "
                  "(0x%lx, IRQs %u,%u)", reg_phys, irqs[0], irqs[1]);
    }

    auto* drv = create_bcm_genet(reg_phys, reg_size, irqs[0], irqs[1]);
    if (!drv) {
        log::error("platform: failed to create GENET driver");
        return -1;
    }

    // Spawn a kernel task for the driver. attach() runs inside the task
    // (not here) so we don't block the boot thread during slow hardware
    // initialization (MDIO scan, PHY reset, DMA allocation, etc.).
    sched::task* t = sched::create_kernel_task(
        platform_task_entry, drv, drv->name());
    if (!t) {
        log::error("platform: task creation failed for %s", drv->name());
        heap::ufree_delete(drv);
        return -1;
    }

    drv->set_task(t);
    sched::enqueue(t);
    log::info("platform: GENET driver task spawned (attach will run asynchronously)");
    return 0;
}

__PRIVILEGED_CODE int32_t platform_init() {
    uint32_t bound = 0;

    if (probe_genet() == 0) {
        bound++;
    }

    if (bound > 0) {
        log::info("platform: %u device(s) bound", bound);
    }

    return 0;
}

} // namespace drivers
