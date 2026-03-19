#include "drivers/usb/core/usb_transfer.h"
#include "drivers/usb/xhci/xhci.h"

namespace usb {

int32_t control_transfer(device* dev,
                         uint8_t request_type, uint8_t request,
                         uint16_t value, uint16_t index,
                         void* data, uint16_t length) {
    if (!dev || !dev->hcd || !dev->hcd_device) {
        return -1;
    }

    auto* hcd = static_cast<drivers::xhci_hcd*>(dev->hcd);
    auto* xdev = static_cast<drivers::xhci::xhci_device*>(dev->hcd_device);
    return hcd->usb_control_transfer(xdev, request_type, request,
                                     value, index, data, length);
}

int32_t interrupt_transfer(device* dev, uint8_t endpoint_addr,
                           void* buffer, uint32_t length) {
    if (!dev || !dev->hcd || !dev->hcd_device) {
        return -1;
    }

    auto* hcd = static_cast<drivers::xhci_hcd*>(dev->hcd);
    auto* xdev = static_cast<drivers::xhci::xhci_device*>(dev->hcd_device);
    return hcd->usb_submit_transfer(xdev, endpoint_addr, buffer, length);
}

int32_t bulk_transfer(device* dev, uint8_t endpoint_addr,
                      void* buffer, uint32_t length) {
    if (!dev || !dev->hcd || !dev->hcd_device) {
        return -1;
    }

    auto* hcd = static_cast<drivers::xhci_hcd*>(dev->hcd);
    auto* xdev = static_cast<drivers::xhci::xhci_device*>(dev->hcd_device);
    return hcd->usb_submit_transfer(xdev, endpoint_addr, buffer, length);
}

} // namespace usb
