#ifndef XHCI_USB_DEVICE_DRIVER_H
#define XHCI_USB_DEVICE_DRIVER_H
#include "xhci.h"
#include "xhci_device.h"
#include "xhci_usb_interface.h"

using xhci_hcd = modules::xhci_driver_module;

class xhci_usb_device_driver {
public:
    xhci_usb_device_driver() = default;
    ~xhci_usb_device_driver() = default;

    void attach_interface(xhci_usb_interface* interface);

    virtual void on_startup(xhci_hcd* hcd, xhci_device* dev) = 0;
    virtual void on_event(xhci_hcd* hcd, xhci_device* dev) = 0;

protected:
    xhci_usb_interface* m_interface;
};

#endif // XHCI_USB_DEVICE_DRIVER_H
