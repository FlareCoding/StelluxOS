#ifndef XHCI_ENDPOINT_H
#define XHCI_ENDPOINT_H
#include "xhci_device_ctx.h"
#include <modules/usb/usb_descriptors.h>

class xhci_endpoint {
public:
    xhci_endpoint(uint8_t xhc_slot_id, const usb_endpoint_descriptor* desc);
    ~xhci_endpoint() = default;

    uint8_t     usb_endpoint_addr;
    uint8_t     usb_endpoint_attributes;
    uint16_t    max_packet_size;
    uint8_t     interval;
    uint8_t     xhc_endpoint_type;
    uint8_t     xhc_endpoint_num;

    __force_inline__ uint8_t* get_data_buffer() { return m_data_buffer; }
    __force_inline__ uintptr_t get_data_buffer_dma() { return m_data_buffer_dma_addr; }

    __force_inline__ xhci_transfer_ring* get_transfer_ring() { 
        return m_transfer_ring.get();
    }

private:
    uint8_t*    m_data_buffer;
    uintptr_t   m_data_buffer_dma_addr;
    kstl::shared_ptr<xhci_transfer_ring> m_transfer_ring;

    void _allocate_internal_data_buffer();
};

#endif // XHCI_ENDPOINT_H
