#ifndef XHCI_USB_HID_DRIVER_H
#define XHCI_USB_HID_DRIVER_H
#include "xhci_usb_device_driver.h"

class xhci_usb_hid_driver : public xhci_usb_device_driver {
public:
    xhci_usb_hid_driver() = default;
    ~xhci_usb_hid_driver() = default;

    virtual void on_event(uint8_t* data) = 0;

    void on_startup(xhci_hcd* hcd, xhci_device* dev) override;
    void on_event(xhci_hcd* hcd, xhci_device* dev) override;

private:
    void _request_hid_report(xhci_hcd* hcd, xhci_device* dev);
};

#endif // XHCI_USB_HID_DRIVER_H
