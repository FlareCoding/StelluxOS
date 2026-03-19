#ifndef STELLUX_DRIVERS_USB_XHCI_XHCI_ENDPOINT_H
#define STELLUX_DRIVERS_USB_XHCI_XHCI_ENDPOINT_H

#include "xhci_rings.h"
#include "drivers/usb/usb_descriptors.h"

namespace drivers::xhci {

class xhci_endpoint {
public:
    xhci_endpoint() = default;
    int32_t init(uint8_t slot_id, const usb::usb_endpoint_descriptor* desc);
    void destroy();

    // USB endpoint properties
    inline uint8_t  endpoint_addr() const { return m_endpoint_addr; }
    inline uint8_t  endpoint_num() const { return m_endpoint_addr & 0x0F; }
    inline bool     is_in() const { return (m_endpoint_addr & 0x80) != 0; }
    inline uint8_t  transfer_type() const { return m_attributes & 0x03; }
    inline uint16_t max_packet_size() const { return m_max_packet_size; }
    inline uint8_t  interval() const { return m_interval; }

    // xHCI properties
    inline uint8_t  dci() const { return m_dci; }
    inline uint8_t  xhc_ep_type() const { return m_xhc_ep_type; }

    // Transfer ring
    inline xhci_transfer_ring* ring() { return m_ring; }

private:
    uint8_t   m_endpoint_addr = 0;
    uint8_t   m_attributes = 0;
    uint16_t  m_max_packet_size = 0;
    uint8_t   m_interval = 0;
    uint8_t   m_dci = 0;             // Device Context Index (1-31)
    uint8_t   m_xhc_ep_type = 0;     // xHCI endpoint type for context programming

    xhci_transfer_ring* m_ring = nullptr;
};

} // namespace drivers::xhci

#endif // STELLUX_DRIVERS_USB_XHCI_XHCI_ENDPOINT_H
