#ifndef STELLUX_DRIVERS_USB_XHCI_XHCI_DEVICE_H
#define STELLUX_DRIVERS_USB_XHCI_XHCI_DEVICE_H

#include "xhci_device_ctx.h"
#include "xhci_rings.h"
#include "xhci_endpoint.h"

namespace drivers::xhci {

class xhci_device {
public:
    xhci_device() = default;
    int32_t init(uint8_t port_id, uint8_t slot_id, uint8_t speed, bool csz);
    void destroy();

    // Identity
    inline uint8_t port_id() const { return m_port_id; }
    inline uint8_t slot_id() const { return m_slot_id; }
    inline uint8_t speed() const { return m_speed; }

    // Input context (DMA, used for Address Device / Configure Endpoint commands)
    inline uintptr_t input_ctx_phys() const { return m_input_ctx_phys; }

    xhci_input_control_context32* input_ctrl_ctx();
    xhci_slot_context32*          input_slot_ctx();
    xhci_endpoint_context32*      input_ctrl_ep_ctx();
    xhci_endpoint_context32*      input_ep_ctx(uint8_t ep_num);

    // Output device context (set by xhci_hcd after writing DCBAA[slot_id])
    inline void  set_output_ctx(void* ctx) { m_output_ctx = ctx; }
    inline void* output_ctx() const { return m_output_ctx; }

    // Copy output device context into the input context's device context portion
    void sync_input_ctx();

    // Control transfer ring
    inline xhci_transfer_ring* ctrl_ring() { return m_ctrl_ring; }

    // Non-control endpoints (indexed by DCI)
    xhci_endpoint* endpoint(uint8_t dci) { return m_endpoints[dci]; }
    void set_endpoint(uint8_t dci, xhci_endpoint* ep) { m_endpoints[dci] = ep; }

private:
    uint8_t   m_port_id = 0;     // 1-based port ID
    uint8_t   m_slot_id = 0;     // Slot index in the DCBAA
    uint8_t   m_speed = 0;       // Port speed
    bool      m_csz = false;     // 64-byte context size (from HCCPARAMS1.CSZ)

    void*     m_input_ctx = nullptr;
    uintptr_t m_input_ctx_phys = 0;

    void*     m_output_ctx = nullptr;

    xhci_transfer_ring* m_ctrl_ring = nullptr;

    // Non-control endpoints (DCI 2-31, index 0-1 unused)
    static constexpr uint8_t MAX_ENDPOINTS = 31;
    xhci_endpoint* m_endpoints[MAX_ENDPOINTS + 1] = {};
};

} // namespace drivers::xhci

#endif // STELLUX_DRIVERS_USB_XHCI_XHCI_DEVICE_H
