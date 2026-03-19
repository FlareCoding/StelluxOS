#ifndef STELLUX_DRIVERS_USB_CORE_USB_DEVICE_H
#define STELLUX_DRIVERS_USB_CORE_USB_DEVICE_H

#include "common/types.h"

namespace usb {

struct endpoint {
    uint8_t  address;         // bEndpointAddress (e.g. 0x81 = EP1 IN)
    uint8_t  transfer_type;   // 0=control, 1=isochronous, 2=bulk, 3=interrupt
    uint16_t max_packet_size;
    uint8_t  interval;        // polling interval (interrupt/isochronous)

    inline uint8_t number() const { return address & 0x0F; }
    inline bool is_in() const { return (address & 0x80) != 0; }
};

struct interface {
    uint8_t  interface_number;
    uint8_t  alternate_setting;
    uint8_t  interface_class;
    uint8_t  interface_subclass;
    uint8_t  interface_protocol;
    uint8_t  num_endpoints;
    endpoint endpoints[16];
};

struct device {
    // Identity (from device descriptor)
    uint16_t vid;
    uint16_t pid;
    uint16_t bcd_usb;
    uint8_t  device_class;
    uint8_t  device_subclass;
    uint8_t  device_protocol;
    uint8_t  speed;

    // Active configuration
    uint8_t  config_value;
    uint8_t  num_interfaces;
    interface interfaces[16];

    // Hub topology
    bool     is_hub;
    uint8_t  hub_num_ports;

    // Opaque handles for the USB Core transfer API
    void* hcd;          // host controller driver instance (xhci_hcd*)
    void* hcd_device;   // HCD-private device handle (xhci_device*)
};

} // namespace usb

#endif // STELLUX_DRIVERS_USB_CORE_USB_DEVICE_H
