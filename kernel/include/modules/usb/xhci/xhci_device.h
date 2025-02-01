#ifndef XHCI_DEVICE_H
#define XHCI_DEVICE_H
#include "xhci_usb_interface.h"

class xhci_device {
public:
    // port is a 1-based port ID
    xhci_device(uint8_t port, uint8_t slot, uint8_t speed, bool use_64byte_ctx);
    ~xhci_device() = default;

    __force_inline__ uint8_t get_port_id() const { return m_port_id; }
    __force_inline__ uint8_t get_port_regset_id() const { return m_port_id - 1; }
    __force_inline__ uint8_t get_slot_id() const { return m_slot_id; }
    __force_inline__ uint8_t get_speed() const { return m_speed; }

    __force_inline__ uintptr_t get_input_ctx_dma() const { return m_input_ctx_dma_addr; }
    
    __force_inline__ xhci_transfer_ring* get_ctrl_ep_transfer_ring() { 
        return m_ctrl_ep_ring.get();
    }

    xhci_input_control_context32*   get_input_ctrl_ctx();
    xhci_slot_context32*            get_input_slot_ctx();
    xhci_endpoint_context32*        get_input_ctrl_ep_ctx();
    xhci_endpoint_context32*        get_input_ep_ctx(uint8_t endpoint_num);

    // Copies data from the output device context into the input context
    void sync_input_ctx(void* out_ctx);

    void setup_add_interface(const usb_interface_descriptor* desc);

    kstl::vector<kstl::shared_ptr<xhci_usb_interface>> interfaces;

private:
    const uint8_t m_port_id;    // 1-based port ID
    const uint8_t m_slot_id;    // slot index in the xhci DCBAA
    const uint8_t m_speed;      // port speed
    const bool    m_use64byte_ctx;

    void*         m_input_ctx;
    uintptr_t     m_input_ctx_dma_addr;
    kstl::shared_ptr<xhci_transfer_ring> m_ctrl_ep_ring;

private:
    void _alloc_input_ctx();
    void _alloc_ctrl_ep_ring();
};

#endif // XHCI_DEVICE_H
