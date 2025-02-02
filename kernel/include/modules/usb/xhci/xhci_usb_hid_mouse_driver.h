#ifndef XHCI_USB_HID_MOUSE_DRIVER_H
#define XHCI_USB_HID_MOUSE_DRIVER_H
#include "xhci_usb_hid_driver.h"
#include <modules/usb/hid/hid_mouse_report_parser.h>

class xhci_usb_hid_mouse_driver : public xhci_usb_hid_driver {
public:
    xhci_usb_hid_mouse_driver() = default;
    ~xhci_usb_hid_mouse_driver() = default;

    void on_device_init() override;
    void on_device_event(uint8_t* data) override;

private:
    hid_mouse_report_layout m_report_layout;

    void _construct_default_report_layout();
};

#endif // XHCI_USB_HID_MOUSE_DRIVER_H
