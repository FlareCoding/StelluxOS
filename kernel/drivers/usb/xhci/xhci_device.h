#ifndef STELLUX_DRIVERS_USB_XHCI_XHCI_DEVICE_H
#define STELLUX_DRIVERS_USB_XHCI_XHCI_DEVICE_H

#include "xhci_device_ctx.h"
#include "xhci_rings.h"
#include "xhci_endpoint.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"
#include "sync/mutex.h"

namespace usb { struct device; }

namespace drivers::xhci {

struct xhci_interface_info {
    uint8_t interface_number = 0;
    uint8_t alternate_setting = 0;
    uint8_t interface_class = 0;
    uint8_t interface_subclass = 0;
    uint8_t interface_protocol = 0;
    uint16_t hid_report_desc_length = 0;
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

    // Hub topology (set before Address Device for hub-downstream devices)
    inline uint32_t route_string() const { return m_route_string; }
    inline uint8_t  parent_slot_id() const { return m_parent_slot_id; }
    inline uint8_t  parent_port_num() const { return m_parent_port_num; }
    inline uint8_t  root_port_id() const { return m_root_port_id; }
    inline bool     is_hub() const { return m_is_hub; }
    inline uint8_t  hub_num_ports() const { return m_hub_num_ports; }
    inline uint8_t  tt_think_time() const { return m_tt_think_time; }
    inline bool     mtt() const { return m_mtt; }

    void set_route_string(uint32_t rs) { m_route_string = rs; }
    void set_parent_slot_id(uint8_t s) { m_parent_slot_id = s; }
    void set_parent_port_num(uint8_t p) { m_parent_port_num = p; }
    void set_root_port_id(uint8_t rp) { m_root_port_id = rp; }
    void set_is_hub(bool h) { m_is_hub = h; }
    void set_hub_num_ports(uint8_t n) { m_hub_num_ports = n; }
    void set_tt_think_time(uint8_t t) { m_tt_think_time = t; }
    void set_mtt(bool m) { m_mtt = m; }

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

    // EP0 transfer mutex — serializes enqueue+doorbell+wait across tasks
    inline sync::mutex& ctrl_transfer_mutex() { return m_ctrl_transfer_mutex; }

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

    // Lookup endpoint by USB endpoint address (e.g. 0x81 = EP1 IN)
    xhci_endpoint* endpoint_by_address(uint8_t address) {
        uint8_t ep_num = address & 0x0F;
        bool is_in = (address & 0x80) != 0;
        uint8_t dci = static_cast<uint8_t>(ep_num * 2 + (is_in ? 1 : 0));
        if (dci < 2 || dci > MAX_ENDPOINTS) return nullptr;
        return m_endpoints[dci];
    }

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

    // USB-core identity for disconnect/finalize, independent of slot reuse.
    inline usb::device* core_device() const { return m_core_device; }
    inline void set_core_device(usb::device* dev) { m_core_device = dev; }

    // Hub downstream device tracking (hub_port is 1-based, array is 0-based)
    static constexpr uint8_t MAX_HUB_PORTS = 16;
    xhci_device* hub_child(uint8_t hub_port) {
        if (hub_port < 1 || hub_port > MAX_HUB_PORTS) return nullptr;
        return m_hub_children[hub_port - 1];
    }
    void set_hub_child(uint8_t hub_port, xhci_device* child) {
        if (hub_port >= 1 && hub_port <= MAX_HUB_PORTS) {
            m_hub_children[hub_port - 1] = child;
        }
    }

private:
    uint8_t   m_port_id = 0;     // 1-based port ID (root hub port for root devices)
    uint8_t   m_slot_id = 0;     // Slot index in the DCBAA
    uint8_t   m_speed = 0;       // Port speed
    bool      m_csz = false;     // 64-byte context size (from HCCPARAMS1.CSZ)

    // Hub topology tracking
    uint32_t  m_route_string = 0;      // xHCI route string for this device
    uint8_t   m_parent_slot_id = 0;    // Slot ID of parent hub (0 = root hub)
    uint8_t   m_parent_port_num = 0;   // Port number on parent hub (0 = root hub)
    uint8_t   m_root_port_id = 0;      // Root hub port ID this device is ultimately behind
    bool      m_is_hub = false;        // True if this device is a hub
    uint8_t   m_hub_num_ports = 0;     // Number of downstream ports (hubs only)
    uint8_t   m_tt_think_time = 0;     // TT think time (HS hubs only)
    bool      m_mtt = false;           // Multi-TT support

    void*     m_input_ctx = nullptr;
    uintptr_t m_input_ctx_phys = 0;

    void*     m_output_ctx = nullptr;

    void*     m_ctrl_transfer_buffer = nullptr;
    uintptr_t m_ctrl_transfer_buffer_phys = 0;

    xhci_transfer_ring* m_ctrl_ring = nullptr;

    // EP0 transfer mutex — protects enqueue+doorbell+wait against concurrent callers
    sync::mutex m_ctrl_transfer_mutex;

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

    // USB core backpointer, set once the logical usb::device is published.
    usb::device* m_core_device = nullptr;

    // Hub downstream devices (indexed by hub_port - 1)
    xhci_device* m_hub_children[MAX_HUB_PORTS] = {};
};

} // namespace drivers::xhci

#endif // STELLUX_DRIVERS_USB_XHCI_XHCI_DEVICE_H
