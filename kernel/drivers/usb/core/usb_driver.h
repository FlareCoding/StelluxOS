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

constexpr uint8_t USB_MATCH_ANY = 0xFF;

struct class_match {
    uint8_t interface_class;     // USB_MATCH_ANY = wildcard
    uint8_t interface_subclass;  // USB_MATCH_ANY = wildcard
    uint8_t interface_protocol;  // USB_MATCH_ANY = wildcard
};

class class_driver {
public:
    class_driver(const char* name) : m_name(name) {}
    virtual ~class_driver() = default;

    class_driver(const class_driver&) = delete;
    class_driver& operator=(const class_driver&) = delete;

    // Called after the Core matches an interface to this driver.
    // The driver should perform USB setup (SET_PROTOCOL, SET_IDLE, etc.)
    // and return 0 to claim the interface, or negative to decline.
    virtual int32_t probe(device* dev, interface* iface) = 0;

    // Driver main loop, executed in the driver's own kernel task.
    virtual void run() = 0;

    // Called when the device is disconnected. The driver should stop
    // any in-progress work and prepare to exit run().
    virtual void disconnect() {}

    const char* name() const { return m_name; }
    sched::task* task() const { return m_task; }
    device* bound_device() const { return m_bound_device; }
    uint8_t bound_slot_id() const { return m_bound_slot_id; }
    uint8_t bound_interface_index() const { return m_bound_interface_index; }

protected:
    const char* m_name;
    sched::task* m_task = nullptr;
    device* m_bound_device = nullptr;
    uint8_t m_bound_slot_id = 0;
    uint8_t m_bound_interface_index = 0xff;

    friend void core::device_configured(drivers::xhci_hcd*, drivers::xhci::xhci_device*,
                                        const usb_device_descriptor&);
};

using class_driver_factory = class_driver* (*)(device* dev, interface* iface);

struct class_driver_entry {
    class_match match;
    class_driver_factory create;
    const char* name;
};

} // namespace usb

#define USB_MATCH(cls, sub, proto) \
    { cls, sub, proto }

#define USB_CLASS_DRIVER_FACTORY(drv) \
    [](usb::device* dev, usb::interface* iface) -> usb::class_driver* { \
        return heap::ualloc_new<drv>(dev, iface); \
    }

#define REGISTER_USB_CLASS_DRIVER(drv, match, factory) \
    __attribute__((used, section(".usb_class_drivers"))) \
    static const ::usb::class_driver_entry _usb_class_reg_##drv = { \
        match, factory, #drv \
    }

#endif // STELLUX_DRIVERS_USB_CORE_USB_DRIVER_H
