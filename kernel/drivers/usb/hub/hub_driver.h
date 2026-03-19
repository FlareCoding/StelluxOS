#ifndef STELLUX_DRIVERS_USB_HUB_HUB_DRIVER_H
#define STELLUX_DRIVERS_USB_HUB_HUB_DRIVER_H

#include "drivers/usb/core/usb_driver.h"
#include "drivers/usb/hub/hub_descriptors.h"

namespace usb::hub {

class hub_driver : public usb::class_driver {
public:
    hub_driver(usb::device* dev, usb::interface* iface);

    int32_t probe(usb::device* dev, usb::interface* iface) override;
    void run() override;
    void disconnect() override;

private:
    usb::device*    m_dev;
    usb::interface* m_iface;
    hub_descriptor  m_hub_desc;
    bool            m_disconnected = false;

    // Hub class requests
    int32_t get_hub_descriptor(hub_descriptor* out);
    int32_t get_port_status(uint8_t port, hub_port_status* out);
    int32_t set_port_feature(uint8_t port, uint16_t feature);
    int32_t clear_port_feature(uint8_t port, uint16_t feature);

    // Port management
    void power_on_ports();
    void handle_port_change(uint8_t port);
    int32_t reset_port(uint8_t port);
    uint8_t find_interrupt_in_endpoint() const;

    // Convert hub port status speed bits to xHCI speed encoding
    static uint8_t hub_speed_to_xhci_speed(uint16_t port_status);
};

} // namespace usb::hub

#endif // STELLUX_DRIVERS_USB_HUB_HUB_DRIVER_H
