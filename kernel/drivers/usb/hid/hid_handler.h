#ifndef STELLUX_DRIVERS_USB_HID_HID_HANDLER_H
#define STELLUX_DRIVERS_USB_HID_HID_HANDLER_H

#include "drivers/usb/hid/hid_parser.h"

namespace usb::hid {

// Base interface for device-type-specific report processing.
// The HID driver instantiates the appropriate handler after
// parsing the report descriptor and detecting usage pages.
class hid_handler {
public:
    virtual ~hid_handler() = default;

    // Called once after the report descriptor is parsed, before
    // the interrupt transfer loop begins. The handler should
    // locate its fields of interest in the layout.
    virtual void init(const report_layout& layout) = 0;

    // Called each time a new HID report arrives from the device.
    // data points to the raw interrupt IN buffer, length is
    // the number of bytes received.
    virtual void on_report(const uint8_t* data, uint32_t length) = 0;
};

} // namespace usb::hid

#endif // STELLUX_DRIVERS_USB_HID_HID_HANDLER_H
