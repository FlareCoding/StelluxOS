#ifndef STELLUX_DRIVERS_PCI_DRIVER_H
#define STELLUX_DRIVERS_PCI_DRIVER_H

#include "pci/pci.h"
#include "drivers/device_driver.h"
#include "sync/wait_queue.h"
#include "mm/heap.h"

namespace drivers {

constexpr int32_t OK               = 0;
constexpr int32_t ERR_INVALID_BAR  = -1;
constexpr int32_t ERR_BAR_NOT_MMIO = -2;

/**
 * Match criteria for binding a PCI driver to a device.
 * Fields set to their wildcard value match any device.
 */
struct pci_match_id {
    uint16_t vendor;     // PCI_MATCH_ANY = wildcard
    uint16_t device;     // PCI_MATCH_ANY = wildcard
    uint8_t  class_code; // PCI_MATCH_ANY_8 = wildcard
    uint8_t  subclass;   // PCI_MATCH_ANY_8 = wildcard
    uint8_t  prog_if;    // PCI_MATCH_ANY_8 = wildcard
};

constexpr uint16_t PCI_MATCH_ANY   = 0xFFFF;
constexpr uint8_t  PCI_MATCH_ANY_8 = 0xFF;

class pci_driver;
using pci_driver_factory = pci_driver* (*)(pci::device* dev);

/**
 * Static registration entry placed in the .pci_drivers linker section.
 * The framework iterates these during PCI enumeration to find matching
 * drivers. Only the first successful match per device is used.
 */
struct pci_driver_entry {
    pci_match_id id;
    pci_driver_factory create;
    const char* name;
};

/**
 * Initializes the PCI driver framework: enumerate devices, match against
 * registered drivers, instantiate and attach, then spawn kernel tasks.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * PCI device driver base. Extends device_driver with PCI bus mechanics:
 * device reference, BAR mapping, MSI/MSI-X setup, and interrupt dispatch.
 *
 * Concrete PCI drivers (XHCI, NIC, etc.) inherit from this class.
 *
 * Factory contract: driver factories registered via REGISTER_PCI_DRIVER
 * must allocate from the unprivileged heap (heap::ualloc_new).
 * On failure, the factory must return nullptr without leaking memory.
 * The framework frees driver objects with heap::ufree_delete on error.
 */
class pci_driver : public device_driver {
public:
    pci_driver(const char* name, pci::device* dev)
        : device_driver(name), m_dev(dev), m_event_pending(false) {
        m_irq_wq.init();
        m_irq_lock = sync::SPINLOCK_INIT;
        for (uint8_t i = 0; i < pci::MAX_BARS; i++) {
            m_bar_mappings[i] = { 0, 0 };
        }
    }

    pci::device& dev() const { return *m_dev; }

    /**
     * Called from interrupt context when an MSI/MSI-X vector fires.
     * @param vector 0-based MSI vector index relative to this driver's allocation.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE virtual void on_interrupt(uint32_t vector) = 0;

    /**
     * Tears down MSI and unmaps all BARs. Concrete drivers that override
     * this should call pci_driver::detach() at the end of their override.
     * @return 0 on success, negative error code on failure.
     */
    int32_t detach() override;

protected:
    /**
     * Map a BAR into kernel virtual address space. Idempotent: returns
     * the existing mapping if the BAR is already mapped.
     * @param index BAR index (0-5).
     * @param out_va Receives the mapped virtual address.
     * @return 0 on success, negative error code on failure.
     */
    int32_t map_bar(uint8_t index, uintptr_t& out_va);

    /**
     * Unmap a previously mapped BAR. No-op if not mapped.
     * @param index BAR index (0-5).
     * @return 0 on success, negative error code on failure.
     */
    int32_t unmap_bar(uint8_t index);

    /**
     * Set up MSI with the given vector count. Calls pci::device::enable_msi
     * and registers the framework ISR for each vector.
     * @param count Requested number of vectors (rounded to power of two).
     * @return 0 on success, negative error code on failure.
     */
    int32_t setup_msi(uint32_t count);

    /**
     * Set up MSI-X with the given vector count. Same as setup_msi but
     * uses MSI-X.
     * @param count Requested number of vectors.
     * @return 0 on success, negative error code on failure.
     */
    int32_t setup_msix(uint32_t count);

    /**
     * Block the driver task until an interrupt or other event wakes it.
     * Multiple interrupts may coalesce into a single wake, the driver's
     * run() loop must drain all pending device work on each return.
     */
    void wait_for_event();

    pci::device* m_dev;
    sync::wait_queue m_irq_wq;
    sync::spinlock m_irq_lock;
    bool m_event_pending;

    struct bar_mapping {
        uintptr_t base; // for vmm::free()
        uintptr_t va;   // for MMIO access
    };
    bar_mapping m_bar_mappings[pci::MAX_BARS];

private:
    int32_t register_msi_handlers();

    /** @note Privilege: **required** */
    __PRIVILEGED_CODE static void isr_trampoline(uint32_t vector, void* context);

    /** @note Privilege: **required** */
    __PRIVILEGED_CODE void notify_interrupt(uint32_t global_vector);

    /** @note Privilege: **required** */
    friend __PRIVILEGED_CODE int32_t init();
};

} // namespace drivers

/**
 * Build a pci_match_id. Wraps the braced initializer in parentheses so
 * the preprocessor treats the commas as part of a single macro argument.
 */
#define PCI_MATCH(vendor, device, cls, sub, prog) \
    { vendor, device, cls, sub, prog }

/**
 * Default factory for PCI drivers. Allocates from the unprivileged heap.
 */
#define PCI_DRIVER_FACTORY(drv) \
    [](pci::device* dev) -> ::drivers::pci_driver* { \
        return heap::ualloc_new<drv>(dev); \
    }

/**
 * Register a PCI driver. Place in a source file alongside the driver class.
 * The driver framework discovers these entries during PCI bus enumeration.
 *
 * Usage:
 *   REGISTER_PCI_DRIVER(xhci_driver,
 *       PCI_MATCH(PCI_MATCH_ANY, PCI_MATCH_ANY, 0x0C, 0x03, 0x30),
 *       PCI_DRIVER_FACTORY(xhci_driver));
 */
#define REGISTER_PCI_DRIVER(drv, match, factory) \
    __attribute__((used, section(".pci_drivers"))) \
    static const ::drivers::pci_driver_entry _pci_reg_##drv = { \
        match, factory, #drv \
    }

#endif // STELLUX_DRIVERS_PCI_DRIVER_H
