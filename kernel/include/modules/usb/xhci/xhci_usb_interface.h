#ifndef XHCI_USB_INTERFACE_H
#define XHCI_USB_INTERFACE_H
#include "xhci_endpoint.h"
#include <modules/usb/usb_descriptors.h>

class xhci_usb_device_driver;

class xhci_usb_interface {
public:
    xhci_usb_interface(uint8_t dev_slot_id, const usb_interface_descriptor* desc);
    ~xhci_usb_interface() = default;

    void setup_add_endpoint(const usb_endpoint_descriptor* ep_desc);

    usb_interface_descriptor                        descriptor;
    kstl::vector<kstl::shared_ptr<xhci_endpoint>>   endpoints;
    xhci_usb_device_driver*                         driver;

private:
    uint8_t m_dev_slot_id;
};

#endif // XHCI_USB_INTERFACE_H
