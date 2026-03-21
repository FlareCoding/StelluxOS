#ifndef STELLUX_DRIVERS_USB_CORE_USB_CORE_H
#define STELLUX_DRIVERS_USB_CORE_USB_CORE_H

#include "drivers/usb/usb_descriptors.h"

namespace drivers { class xhci_hcd; }
namespace drivers::xhci { class xhci_device; }

namespace usb::core {

// Called by the HCD after a device is fully configured (endpoints created,
// SET_CONFIGURATION sent). Builds a usb::device, matches interfaces against
// registered class drivers, and spawns kernel tasks for bound drivers.
void device_configured(drivers::xhci_hcd* hcd,
                       drivers::xhci::xhci_device* xdev,
                       const usb::usb_device_descriptor& desc);

// Called by the HCD when a device is being torn down (disconnect or detach).
// Notifies bound class drivers via disconnect() and marks the device pending
// teardown. Final release happens later once both the HCD and the class-driver
// tasks have dropped their references.
void device_disconnected(drivers::xhci_hcd* hcd,
                         drivers::xhci::xhci_device* xdev);

void finalize_disconnected_device(drivers::xhci_hcd* hcd,
                                  drivers::xhci::xhci_device* xdev);

} // namespace usb::core

#endif // STELLUX_DRIVERS_USB_CORE_USB_CORE_H
