#ifndef XHCI_USB_INTERFACE_H
#define XHCI_USB_INTERFACE_H
#include "xhci_endpoint.h"
#include <drivers/usb/usb_descriptors.h>

class xhci_usb_device_driver;

class xhci_usb_interface {
public:
    xhci_usb_interface(uint8_t dev_slot_id, const usb_interface_descriptor* desc);
    ~xhci_usb_interface() = default;

    void setup_add_endpoint(const usb_endpoint_descriptor* ep_desc);

    usb_interface_descriptor                        descriptor;
    kstl::vector<kstl::shared_ptr<xhci_endpoint>>   endpoints;
    xhci_usb_device_driver*                         driver;

    // HID report data for HID devices
    uint8_t* additional_data = nullptr;
    size_t   additional_data_length = 0;

private:
    uint8_t m_dev_slot_id;
};

#endif // XHCI_USB_INTERFACE_H
