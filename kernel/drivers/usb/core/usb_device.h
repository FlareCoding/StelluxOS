#ifndef STELLUX_DRIVERS_USB_CORE_USB_DEVICE_H
#define STELLUX_DRIVERS_USB_CORE_USB_DEVICE_H

#include "sync/spinlock.h"

namespace usb {

class class_driver;

constexpr uint8_t MAX_INTERFACES = 16;

// A string descriptor holds at most 126 UTF-16 code units
constexpr size_t MAX_STRING_LENGTH = 126;

struct endpoint {
    uint8_t  address;         // bEndpointAddress (e.g. 0x81 = EP1 IN)
    uint8_t  transfer_type;   // 0=control, 1=isochronous, 2=bulk, 3=interrupt
    uint16_t max_packet_size;
    uint8_t  interval; // polling interval (interrupt/isochronous)

    inline uint8_t number() const { return address & 0x0F; }
    inline bool is_in() const { return (address & 0x80) != 0; }
};

struct interface {
    uint8_t  interface_number;
    uint8_t  alternate_setting;
    uint8_t  interface_class;
    uint8_t  interface_subclass;
    uint8_t  interface_protocol;
    uint16_t hid_report_desc_length;
    uint8_t  num_endpoints;
    endpoint endpoints[16];
};

struct device {
    // Identity (from device descriptor)
    uint16_t vid;
    uint16_t pid;
    uint16_t bcd_usb;
    uint16_t bcd_device;
    uint8_t  device_class;
    uint8_t  device_subclass;
    uint8_t  device_protocol;
    uint8_t  speed;

    // The device's strings in printable ASCII, empty when the device names none or a read fails, and written
    // only before any driver binds, so they need no lock
    char manufacturer[MAX_STRING_LENGTH + 1];
    char product[MAX_STRING_LENGTH + 1];
    char serial_number[MAX_STRING_LENGTH + 1];

    // Active configuration
    uint8_t  config_value;
    uint8_t  num_interfaces;
    interface interfaces[MAX_INTERFACES];

    // Hub topology
    bool     is_hub;
    uint8_t  hub_num_ports;

    // Opaque handles for the USB Core transfer API
    void* hcd;          // host controller driver instance (xhci_hcd*)
    void* hcd_device;   // HCD-private device handle (xhci_device*)

    // Driver-task lifetime tracking for safe disconnect teardown.
    sync::spinlock lifetime_lock;
    uint8_t active_driver_count;
    bool disconnect_pending;
    bool hcd_teardown_complete; // HCD is done dereferencing hcd_device.
    bool finalize_started; // Final release path has claimed ownership.
    class_driver* bound_drivers[MAX_INTERFACES]; // Each interface's driver, until an unplug takes it or its task ends.
};

} // namespace usb

#endif // STELLUX_DRIVERS_USB_CORE_USB_DEVICE_H
