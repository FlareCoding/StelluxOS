#ifndef STELLUX_DRIVERS_USB_XHCI_XHCI_DEVICE_H
#define STELLUX_DRIVERS_USB_XHCI_XHCI_DEVICE_H

#include "xhci_device_ctx.h"
#include "xhci_rings.h"
#include "xhci_endpoint.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"

namespace drivers::xhci {

struct xhci_interface_info {
    uint8_t interface_number = 0;
    uint8_t alternate_setting = 0;
    uint8_t interface_class = 0;
    uint8_t interface_subclass = 0;
    uint8_t interface_protocol = 0;
    uint8_t num_endpoints = 0;
    uint8_t endpoint_dcis[16] = {};
};

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

    // Persistent DMA buffer for control transfers
    inline void* ctrl_transfer_buffer() const { return m_ctrl_transfer_buffer; }
    inline uintptr_t ctrl_transfer_buffer_phys() const { return m_ctrl_transfer_buffer_phys; }

    // Control transfer ring
    inline xhci_transfer_ring* ctrl_ring() { return m_ctrl_ring; }

    // EP0 completion state accessors
    inline sync::spinlock&   ctrl_completion_lock() { return m_ctrl_completion_lock; }
    inline sync::wait_queue& ctrl_completion_wq() { return m_ctrl_completion_wq; }
    inline bool              ctrl_completed() const { return m_ctrl_completed; }
    inline bool*             ctrl_completed_ptr() { return &m_ctrl_completed; }
    inline void              set_ctrl_completed(bool v) { m_ctrl_completed = v; }
    inline xhci_transfer_completion_trb_t& ctrl_result() { return m_ctrl_result; }

    // Non-control endpoints (indexed by DCI)
    static constexpr uint8_t MAX_ENDPOINTS = 31;
    xhci_endpoint* endpoint(uint8_t dci) { return m_endpoints[dci]; }
    void set_endpoint(uint8_t dci, xhci_endpoint* ep) { m_endpoints[dci] = ep; }

    // Interface tracking
    static constexpr uint8_t MAX_INTERFACES = 16;
    inline uint8_t num_interfaces() const { return m_num_interfaces; }
    inline const xhci_interface_info& interface_info(uint8_t index) const {
        return m_interfaces[index];
    }
    inline xhci_interface_info& interface_info_mut(uint8_t index) {
        return m_interfaces[index];
    }
    inline void set_num_interfaces(uint8_t n) { m_num_interfaces = n; }

private:
    uint8_t   m_port_id = 0;     // 1-based port ID
    uint8_t   m_slot_id = 0;     // Slot index in the DCBAA
    uint8_t   m_speed = 0;       // Port speed
    bool      m_csz = false;     // 64-byte context size (from HCCPARAMS1.CSZ)

    void*     m_input_ctx = nullptr;
    uintptr_t m_input_ctx_phys = 0;

    void*     m_output_ctx = nullptr;

    void*     m_ctrl_transfer_buffer = nullptr;
    uintptr_t m_ctrl_transfer_buffer_phys = 0;

    xhci_transfer_ring* m_ctrl_ring = nullptr;

    // EP0 completion tracking
    sync::wait_queue m_ctrl_completion_wq;
    sync::spinlock   m_ctrl_completion_lock;
    bool             m_ctrl_completed = false;
    xhci_transfer_completion_trb_t m_ctrl_result = {};

    // Non-control endpoints (DCI 2-31, index 0-1 unused)
    xhci_endpoint* m_endpoints[MAX_ENDPOINTS + 1] = {};

    // Interface tracking (populated by _configure_device)
    xhci_interface_info m_interfaces[MAX_INTERFACES] = {};
    uint8_t m_num_interfaces = 0;
};

} // namespace drivers::xhci

#endif // STELLUX_DRIVERS_USB_XHCI_XHCI_DEVICE_H
