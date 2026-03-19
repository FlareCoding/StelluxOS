#ifndef STELLUX_DRIVERS_USB_HID_HID_DRIVER_H
#define STELLUX_DRIVERS_USB_HID_HID_DRIVER_H

#include "drivers/usb/core/usb_driver.h"
#include "drivers/usb/hid/hid_parser.h"
#include "drivers/usb/hid/hid_handler.h"

namespace usb::hid {

class hid_driver : public usb::class_driver {
public:
    hid_driver(usb::device* dev, usb::interface* iface);
    ~hid_driver() override;

    int32_t probe(usb::device* dev, usb::interface* iface) override;
    void run() override;
    void disconnect() override;

private:
    usb::device*    m_dev;
    usb::interface* m_iface;
    report_layout   m_layout;
    hid_handler*    m_handler = nullptr;
    bool            m_disconnected = false;

    // Find the interrupt IN endpoint address from the interface
    uint8_t find_interrupt_in_endpoint() const;

    // Detect device type from the parsed report layout and create the handler
    hid_handler* create_handler();
};

} // namespace usb::hid

#endif // STELLUX_DRIVERS_USB_HID_HID_DRIVER_H
