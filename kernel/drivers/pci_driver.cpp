#include "drivers/pci_driver.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "mm/paging_types.h"
#include "msi/msi.h"
#include "sched/sched.h"
#include "dynpriv/dynpriv.h"
#include "common/logging.h"
#include "percpu/percpu.h"

namespace drivers {

extern "C" const pci_driver_entry __pci_drivers_start[];
extern "C" const pci_driver_entry __pci_drivers_end[];

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void pci_driver::isr_trampoline(uint32_t vector, void* context) {
    static_cast<pci_driver*>(context)->notify_interrupt(vector);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void pci_driver::notify_interrupt(uint32_t global_vector) {
    uint32_t local = global_vector - m_dev->get_msi_state().base_vector;
    on_interrupt(local);
    sync::irq_state irq = sync::spin_lock_irqsave(m_irq_lock);
    m_event_pending = true;
    sync::wake_one(m_irq_wq);
    sync::spin_unlock_irqrestore(m_irq_lock, irq);
}

static void pci_task_entry(void* arg) {
    auto* drv = static_cast<pci_driver*>(arg);
    drv->run();
    sched::exit(0);
}

static bool match_device(const pci_match_id& id, pci::device* dev) {
    if (id.vendor != PCI_MATCH_ANY && id.vendor != dev->vendor_id()) return false;
    if (id.device != PCI_MATCH_ANY && id.device != dev->device_id()) return false;
    if (id.class_code != PCI_MATCH_ANY_8 && id.class_code != dev->class_code()) return false;
    if (id.subclass != PCI_MATCH_ANY_8 && id.subclass != dev->subclass()) return false;
    if (id.prog_if != PCI_MATCH_ANY_8 && id.prog_if != dev->prog_if()) return false;
    return true;
}

int32_t pci_driver::detach() {
    const pci::msi_state& ms = m_dev->get_msi_state();
    if (ms.mode != pci::MSI_MODE_NONE) {
        RUN_ELEVATED(m_dev->disable_msi());
    }
    for (uint8_t i = 0; i < pci::MAX_BARS; i++) {
        unmap_bar(i);
    }
    return 0;
}

int32_t pci_driver::map_bar(uint8_t index, uintptr_t& out_va) {
    if (index >= pci::MAX_BARS) {
        return ERR_INVALID_BAR;
    }

    if (m_bar_mappings[index].base != 0) {
        out_va = m_bar_mappings[index].va;
        return 0;
    }

    const pci::bar& b = m_dev->get_bar(index);
    if (b.type == pci::BAR_NONE || b.type == pci::BAR_IO) {
        return ERR_BAR_NOT_MMIO;
    }

    uintptr_t base = 0;
    uintptr_t va = 0;
    int32_t rc = 0;

    RUN_ELEVATED(
        rc = vmm::map_device(
            static_cast<pmm::phys_addr_t>(b.phys),
            static_cast<size_t>(b.size),
            paging::PAGE_READ | paging::PAGE_WRITE,
            base, va)
    );

    if (rc != vmm::OK) {
        return rc;
    }

    m_bar_mappings[index] = {base, va};
    out_va = va;
    return 0;
}

int32_t pci_driver::unmap_bar(uint8_t index) {
    if (index >= pci::MAX_BARS) {
        return ERR_INVALID_BAR;
    }

    bar_mapping& m = m_bar_mappings[index];
    if (m.base == 0) {
        return 0;
    }

    int32_t rc = 0;
    RUN_ELEVATED(rc = vmm::free(m.base));
    m = { 0, 0 };
    return rc;
}

int32_t pci_driver::register_msi_handlers() {
    const pci::msi_state& ms = m_dev->get_msi_state();
    int32_t rc = 0;

    RUN_ELEVATED({
        for (uint32_t i = 0; i < ms.vector_count; i++) {
            int32_t hr = msi::set_handler(
                ms.base_vector + i, isr_trampoline, this);
            if (hr != msi::OK) {
                m_dev->disable_msi();
                rc = hr;
                break;
            }
        }
    });

    return rc;
}

int32_t pci_driver::setup_msi(uint32_t count) {
    int32_t rc = 0;
    RUN_ELEVATED(rc = m_dev->enable_msi(count, percpu::current_cpu_id()));
    if (rc != pci::OK) {
        return rc;
    }
    return register_msi_handlers();
}

int32_t pci_driver::setup_msix(uint32_t count) {
    int32_t rc = 0;
    RUN_ELEVATED(rc = m_dev->enable_msix(count, percpu::current_cpu_id()));
    if (rc != pci::OK) {
        return rc;
    }
    return register_msi_handlers();
}

void pci_driver::wait_for_event() {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_irq_lock);
        while (!m_event_pending) {
            irq = sync::wait(m_irq_wq, m_irq_lock, irq);
        }
        m_event_pending = false;
        sync::spin_unlock_irqrestore(m_irq_lock, irq);
    });
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
    const pci_driver_entry* reg_start = __pci_drivers_start;
    const pci_driver_entry* reg_end = __pci_drivers_end;
    uint32_t reg_count = static_cast<uint32_t>(reg_end - reg_start);

    if (reg_count == 0) {
        log::info("drivers: no PCI drivers registered");
        return OK;
    }

    log::info("drivers: %u PCI driver(s) registered", reg_count);

    uint32_t bound = 0;

    for (uint32_t i = 0; i < pci::device_count(); i++) {
        pci::device* dev = pci::get_device(i);

        for (const pci_driver_entry* reg = reg_start; reg < reg_end; reg++) {
            if (!match_device(reg->id, dev)) {
                continue;
            }

            log::info("drivers: binding %s to %02x:%02x.%d",
                      reg->name, dev->bus(), dev->slot(), dev->func());

            pci_driver* drv = reg->create(dev);
            if (!drv) {
                log::error("drivers: factory failed for %s", reg->name);
                continue;
            }

            int32_t rc = drv->attach();
            if (rc != 0) {
                log::error("drivers: %s attach failed: %d", drv->name(), rc);
                drv->detach();
                heap::ufree_delete(drv);
                continue;
            }

            sched::task* t = sched::create_kernel_task(
                pci_task_entry, drv, drv->name());
            if (!t) {
                log::error("drivers: task creation failed for %s", drv->name());
                drv->detach();
                heap::ufree_delete(drv);
                continue;
            }

            drv->m_task = t;
            sched::enqueue(t);
            bound++;
            break;
        }
    }

    log::info("drivers: %u device(s) bound", bound);
    return OK;
}

} // namespace drivers
