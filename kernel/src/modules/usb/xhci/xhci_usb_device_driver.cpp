#include <modules/usb/xhci/xhci_usb_device_driver.h>

void xhci_usb_device_driver::attach_interface(xhci_usb_interface* interface) {
    m_interface = interface;
}
