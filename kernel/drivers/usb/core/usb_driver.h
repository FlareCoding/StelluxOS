#ifndef STELLUX_DRIVERS_USB_CORE_USB_DRIVER_H
#define STELLUX_DRIVERS_USB_CORE_USB_DRIVER_H

#include "drivers/usb/core/usb_device.h"
#include "drivers/usb/usb_descriptors.h"

namespace sched { struct task; }
namespace drivers { class xhci_hcd; }
namespace drivers::xhci { class xhci_device; }

namespace usb {

namespace core {
void device_configured(drivers::xhci_hcd*, drivers::xhci::xhci_device*,
                       const usb_device_descriptor&);
} // namespace core

// Outside the 8-bit class, subclass and protocol codes, so an entry can name each of them, 0xFF included
constexpr uint16_t USB_MATCH_ANY = 0x100;

// Fills both IDs of a class entry, so no device entry can name the vendor ID 0xFFFF
constexpr uint16_t USB_MATCH_ANY_ID = 0xFFFF;

struct class_match {
    uint16_t interface_class;     // USB_MATCH_ANY = wildcard
    uint16_t interface_subclass;  // USB_MATCH_ANY = wildcard
    uint16_t interface_protocol;  // USB_MATCH_ANY = wildcard
    uint16_t vendor_id;           // USB_MATCH_ANY_ID for a class entry
    uint16_t product_id;          // USB_MATCH_ANY_ID for a class entry
};

/**
 * A driver bound to one interface of a USB device. The USB core calls
 * `probe()` and `disconnect()` on the HCD task and runs `run()` in a task of
 * its own. On an unplug it calls `disconnect()` at most once, which can be
 * before `run()` starts, while it runs or just after it returns.
 *
 * The driver outlives its `run()` and `disconnect()` calls. Its destructor
 * can run on the HCD task, and neither it nor `disconnect()` may block,
 * since the HCD task serves every device on the controller.
 */
class class_driver {
public:
    class_driver(const char* name) : m_name(name) {}
    virtual ~class_driver() = default;

    class_driver(const class_driver&) = delete;
    class_driver& operator=(const class_driver&) = delete;

    /**
     * Called after the Core matches an interface to this driver.
     * The driver should perform USB setup (SET_PROTOCOL, SET_IDLE, etc.)
     * and return 0 to claim the interface, or negative to decline.
     */
    virtual int32_t probe(device* dev, interface* iface) = 0;

    // Driver main loop, executed in the driver's own kernel task.
    virtual void run() = 0;

    // Called when the device is disconnected. The driver should stop
    // any in-progress work and prepare to exit run().
    virtual void disconnect() {}

    const char* name() const { return m_name; }
    sched::task* task() const { return m_task; }
    device* bound_device() const { return m_bound_device; }
    uint8_t bound_interface_index() const { return m_bound_interface_index; }

protected:
    const char* m_name;
    sched::task* m_task = nullptr;
    device* m_bound_device = nullptr;
    uint8_t m_bound_interface_index = 0xff;

    friend void core::device_configured(drivers::xhci_hcd*, drivers::xhci::xhci_device*,
                                        const usb_device_descriptor&);
    friend struct class_driver_test_access;
};

using class_driver_factory = class_driver* (*)(device* dev, interface* iface);

struct class_driver_entry {
    class_match match;
    class_driver_factory create;
    const char* name;
};

namespace core {
/**
 * Finds the entry in [`first`, `last`) that follows `previous` in binding
 * order and matches `iface` of `dev`. In binding order, device entries, which
 * name a vendor and a product ID, come before class entries, which name
 * neither, each in registration order, so an interface is offered to a driver
 * for its device before any driver matching by class alone.
 * @param previous The entry the last call returned, or nullptr to start.
 * @return The entry, or nullptr when no matching entry follows `previous`.
 */
const class_driver_entry* next_matching_entry(
    const class_driver_entry* first,
    const class_driver_entry* last,
    const class_driver_entry* previous,
    const device& dev,
    const interface& iface
);
} // namespace core

} // namespace usb

#define USB_MATCH(cls, sub, proto) \
    { cls, sub, proto, ::usb::USB_MATCH_ANY_ID, ::usb::USB_MATCH_ANY_ID }

#define USB_MATCH_DEVICE(vendor, product, cls, sub, proto) \
    { cls, sub, proto, vendor, product }

#define USB_CLASS_DRIVER_FACTORY(drv) \
    [](usb::device* dev, usb::interface* iface) -> usb::class_driver* { \
        return heap::ualloc_new<drv>(dev, iface); \
    }

#define USB_CLASS_DRIVER_ENTRY_JOIN(prefix, n) prefix##n
#define USB_CLASS_DRIVER_ENTRY_NAME(drv, n) USB_CLASS_DRIVER_ENTRY_JOIN(_usb_class_reg_##drv##_, n)

// Each use defines an entry of its own, so a driver serving several products registers one entry per product
#define REGISTER_USB_CLASS_DRIVER(drv, match, factory) \
    __attribute__((used, section(".usb_class_drivers"))) \
    static const ::usb::class_driver_entry USB_CLASS_DRIVER_ENTRY_NAME(drv, __COUNTER__) = { \
        match, factory, #drv \
    }

#endif // STELLUX_DRIVERS_USB_CORE_USB_DRIVER_H
