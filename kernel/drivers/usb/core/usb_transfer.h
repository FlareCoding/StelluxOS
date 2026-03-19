#ifndef STELLUX_DRIVERS_USB_CORE_USB_TRANSFER_H
#define STELLUX_DRIVERS_USB_CORE_USB_TRANSFER_H

#include "drivers/usb/core/usb_device.h"

namespace usb {

// Synchronous control transfer on EP0.
// For IN (device-to-host): data is filled by the device.
// For OUT (host-to-device): data is sent to the device.
// Direction is encoded in request_type bit 7 (0=OUT, 1=IN).
// Returns 0 on success, negative on failure.
int32_t control_transfer(device* dev,
                         uint8_t request_type, uint8_t request,
                         uint16_t value, uint16_t index,
                         void* data, uint16_t length);

// Synchronous interrupt transfer on the given endpoint.
// Direction (IN/OUT) is determined by the endpoint address bit 7.
// Blocks until the transfer completes.
// Returns 0 on success, negative on failure.
int32_t interrupt_transfer(device* dev, uint8_t endpoint_addr,
                           void* buffer, uint32_t length);

// Synchronous bulk transfer on the given endpoint.
// Direction (IN/OUT) is determined by the endpoint address bit 7.
// Blocks until the transfer completes.
// Returns 0 on success, negative on failure.
int32_t bulk_transfer(device* dev, uint8_t endpoint_addr,
                      void* buffer, uint32_t length);

} // namespace usb

#endif // STELLUX_DRIVERS_USB_CORE_USB_TRANSFER_H
