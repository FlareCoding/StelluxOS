#ifndef STELLUX_DRIVERS_USB_CORE_USB_CORE_H
#define STELLUX_DRIVERS_USB_CORE_USB_CORE_H

#include "drivers/usb/usb_descriptors.h"

namespace drivers { class xhci_hcd; }
namespace drivers::xhci { class xhci_device; }

namespace usb::core {

/**
 * Called by the HCD after a device is fully configured (endpoints created,
 * SET_CONFIGURATION sent). Builds a usb::device, matches interfaces against
 * registered class drivers, spawns kernel tasks for bound drivers, then lists
 * the device in `/dev/sysinfo/usb`.
 */
void device_configured(drivers::xhci_hcd* hcd,
                       drivers::xhci::xhci_device* xdev,
                       const usb::usb_device_descriptor& desc);

/**
 * Called by the HCD when a device is being torn down (disconnect or detach).
 * Takes the device out of `/dev/sysinfo/usb`, then calls each bound driver's
 * `disconnect()` and marks the device pending teardown. A driver whose `run()` has returned is destroyed here, before
 * the HCD ends the device's transfers. Final release happens later once both
 * the HCD and the class-driver tasks have dropped their references.
 */
void device_disconnected(drivers::xhci_hcd* hcd,
                         drivers::xhci::xhci_device* xdev);

void finalize_disconnected_device(drivers::xhci_hcd* hcd,
                                  drivers::xhci::xhci_device* xdev);

/**
 * Hands out host controller numbers from 1, one per call. Controllers attach
 * one at a time at boot, in PCI order, and each takes its number before any
 * step of its attach that can fail, so the number is its place in that order.
 */
uint8_t next_controller_number();

} // namespace usb::core

#endif // STELLUX_DRIVERS_USB_CORE_USB_CORE_H
