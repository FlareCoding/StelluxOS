#include <drivers/usb/xhci/xhci_usb_interface.h>

xhci_usb_interface::xhci_usb_interface(uint8_t dev_slot_id, const usb_interface_descriptor* desc) : m_dev_slot_id(dev_slot_id) {
    this->descriptor = *desc;
}

void xhci_usb_interface::setup_add_endpoint(const usb_endpoint_descriptor* ep_desc) {
    auto ep = kstl::make_shared<xhci_endpoint>(m_dev_slot_id, ep_desc);
    endpoints.push_back(ep);
}
