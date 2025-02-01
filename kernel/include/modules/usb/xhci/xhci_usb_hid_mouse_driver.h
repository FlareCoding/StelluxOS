#ifndef XHCI_USB_HID_MOUSE_DRIVER_H
#define XHCI_USB_HID_MOUSE_DRIVER_H
#include "xhci_usb_hid_driver.h"

class xhci_usb_hid_mouse_driver : public xhci_usb_hid_driver {
public:
    xhci_usb_hid_mouse_driver() = default;
    ~xhci_usb_hid_mouse_driver() = default;

    void on_event(uint8_t* data) override;
};

#endif // XHCI_USB_HID_MOUSE_DRIVER_H
