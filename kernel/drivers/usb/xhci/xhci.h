#ifndef STELLUX_DRIVERS_USB_XHCI_XHCI_H
#define STELLUX_DRIVERS_USB_XHCI_XHCI_H

#include "drivers/pci_driver.h"
#include "drivers/usb/core/usb_transfer.h"
#include "drivers/usb/xhci/xhci_regs.h"
#include "drivers/usb/xhci/xhci_rings.h"
#include "drivers/usb/xhci/xhci_device.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"

namespace drivers {
namespace xhci {

enum class hub_event_type : uint8_t {
    enumerate,
    disconnect,
};

struct hub_event {
    hub_event_type type;
    uint8_t        hub_slot_id; // Slot ID of the hub device
    uint8_t        hub_port;    // 1-based port number on the hub
    uint8_t        speed;       // Device speed (for enumerate events)
};

static constexpr uint8_t HUB_EVENT_QUEUE_SIZE = 32;

struct xhci_hc_params {
    uint8_t  max_device_slots;
    uint16_t max_interrupters;
    uint8_t  max_ports;
    uint8_t  ist;
    uint8_t  erst_max;
    uint16_t max_scratchpad_buffers;
    uint16_t extended_capabilities_offset;
    bool     ac64;
    bool     csz;       // 64-byte context size
    bool     ppc;       // port power control
};

struct xhci_scratchpad {
    void*     table = nullptr;        // DMA: array of N physical page pointers
    void**    page_vaddrs = nullptr;  // Heap: virtual addresses for cleanup
    uint16_t  count = 0;
};

struct xhci_cmd_state {
    bool pending = false;
    bool completed = false;
    xhci_command_completion_trb_t result = {};
};
} // namespace xhci

class xhci_hcd : public pci_driver {
public:
    xhci_hcd(pci::device* dev) : pci_driver("xhci_hcd", dev) {
        m_hub_event_lock = sync::SPINLOCK_INIT;
    }

    int32_t attach() override;
    int32_t detach() override;
    void run() override;

    /** @note Privilege: **required** */
    __PRIVILEGED_CODE void on_interrupt(uint32_t vector) override;

    // Public transfer API for USB Core
    int32_t usb_control_transfer(xhci::xhci_device* device,
                                 uint8_t request_type, uint8_t request,
                                 uint16_t value, uint16_t index,
                                 void* data, uint16_t length);

    int32_t usb_submit_transfer(xhci::xhci_device* device, uint8_t endpoint_addr,
                                void* buffer, uint32_t length);
    int32_t usb_submit_transfer_async(xhci::xhci_device* device,
                                      usb::transfer_request& request);
    void usb_cancel_transfer(xhci::xhci_device* device,
                             usb::transfer_request& request);
    int32_t usb_open_interrupt_in_stream(xhci::xhci_device* device,
                                         uint8_t endpoint_addr,
                                         uint32_t payload_length);
    int32_t usb_read_interrupt_in_stream(xhci::xhci_device* device,
                                         uint8_t endpoint_addr,
                                         void* buffer,
                                         uint32_t buffer_len,
                                         uint32_t* out_length);
    void usb_close_interrupt_in_stream(xhci::xhci_device* device,
                                       uint8_t endpoint_addr);
    void release_disconnected_device(xhci::xhci_device* device);

    // Queue a hub port enumeration request. Called from the hub driver task.
    // The HCD processes this asynchronously from its own task context.
    void queue_hub_enumerate(xhci::xhci_device* hub_device, uint8_t hub_port, uint8_t speed);

    // Queue a hub port disconnect request. Called from the hub driver task.
    void queue_hub_disconnect(xhci::xhci_device* hub_device, uint8_t hub_port);

    // Update the slot context to reflect that this device is a hub.
    // Called synchronously from the hub driver's probe() which runs on the HCD task.
    int32_t configure_as_hub(xhci::xhci_device* device, uint8_t num_ports, uint8_t tt_think_time, bool mtt);

private:
    // Host controller MMIO virtual base address and mapped size
    uintptr_t m_xhc_base = 0;
    size_t    m_xhc_bar_size = 0;

    // xHCI register sets
    volatile xhci::xhci_capability_registers*   m_xhc_cap_regs = nullptr;
    volatile xhci::xhci_operational_registers*  m_xhc_op_regs = nullptr;
    volatile xhci::xhci_runtime_registers*      m_xhc_runtime_regs = nullptr;

    // Host controller configuration parameters
    xhci::xhci_hc_params m_hc_params = {};

    // Device Context Base Address Array (DMA, entries are physical addresses)
    uint64_t* m_dcbaa = nullptr;

    // Scratchpad buffer resources
    xhci::xhci_scratchpad m_scratchpad;

    // Command ring
    xhci::xhci_command_ring* m_cmd_ring = nullptr;

    // Event ring
    xhci::xhci_event_ring* m_event_ring = nullptr;

    // Doorbell register array
    volatile uint32_t* m_doorbells = nullptr;

    // Pending command state (single in-flight command)
    xhci::xhci_cmd_state m_cmd_state;

    // Per-port device tracking (indexed by 0-based port_index)
    xhci::xhci_device** m_port_devices = nullptr;

    // Per-slot device lookup (indexed by 1-based slot_id, 0 unused)
    xhci::xhci_device* m_slot_devices[256] = {};

    // USB3 port bitmap (bit N = port N is USB3)
    uint32_t m_usb3_port_map[8] = {};

    // Hub event queue (hub driver posts events, HCD processes them)
    xhci::hub_event m_hub_events[xhci::HUB_EVENT_QUEUE_SIZE];
    uint8_t         m_hub_event_head = 0;
    uint8_t         m_hub_event_tail = 0;
    sync::spinlock  m_hub_event_lock;

    // Deferred endpoint doorbells, rung after event processing finishes.
    struct pending_doorbell { uint8_t slot_id; uint8_t target; };
    pending_doorbell m_pending_doorbells[32] = {};
    uint8_t          m_pending_doorbell_count = 0;

private:
    void _parse_extended_capabilities();
    void _request_bios_handoff(volatile uint32_t* usblegsup);
    void _parse_supported_protocol(volatile uint32_t* cap_ptr);

    int32_t _reset_host_controller();
    int32_t _start_host_controller();
    int32_t _stop_host_controller();

    int32_t _configure_operational_registers();
    int32_t _configure_runtime_registers();

    void _process_event_ring();

    // Command submission
    int32_t _send_command(xhci::xhci_trb_t* trb, xhci::xhci_command_completion_trb_t* out = nullptr);

    // Port operations
    int32_t _reset_port(uint8_t port_index);
    void _scan_ports();

    // Doorbell helpers
    void _ring_doorbell(uint8_t slot_id, uint8_t target);
    void _ring_cmd_doorbell();
    uint32_t _read_mfindex() const;
    void _queue_deferred_doorbell(uint8_t slot_id, uint8_t target);

    // Port register helpers (port_index is 0-based)
    volatile uint32_t* _portsc(uint8_t port_index);
    void _write_portsc(uint8_t port_index, uint32_t value);
    void _ack_portsc_changes(uint8_t port_index, uint32_t change_bits);
    bool _is_usb3_port(uint8_t port_index);

private:
    // Shared device setup logic (root hub and hub-connected devices)
    void _enumerate_device(xhci::xhci_device* device);

    // Async interrupt-IN policy helpers
    xhci::endpoint_async_state* _ensure_endpoint_async_state(xhci::xhci_endpoint* ep);
    void _enqueue_pending_request(xhci::endpoint_async_state& state,
                                  usb::transfer_request& request);
    void _finish_request(usb::transfer_request& request,
                         usb::transfer_status status,
                         uint32_t actual_length);
    int32_t _start_async_request(xhci::xhci_device* device,
                                 xhci::xhci_endpoint* ep,
                                 xhci::endpoint_async_state& state,
                                 usb::transfer_request& request,
                                 bool defer_doorbell = false);
    void _kick_async_request_queue(xhci::xhci_device* device,
                                   xhci::xhci_endpoint* ep,
                                   xhci::endpoint_async_state& state,
                                   bool defer_doorbell = false);
    void _complete_async_request(xhci::xhci_device* device,
                                 xhci::xhci_endpoint* ep,
                                 xhci::endpoint_async_state& state,
                                 const xhci::xhci_transfer_completion_trb_t* event);
    int32_t _queue_interrupt_in_stream_td(xhci::xhci_device* device,
                                          xhci::xhci_endpoint* ep,
                                          xhci::endpoint_async_state& state,
                                          bool defer_doorbell = false);
    bool _queue_interrupt_in_stream_payload(xhci::interrupt_in_stream_state& stream,
                                            const uint8_t* data,
                                            uint32_t length,
                                            uint16_t mfindex,
                                            uint64_t queued_t_us,
                                            uint32_t* seq_out = nullptr);
    void _complete_interrupt_in_stream(xhci::xhci_device* device,
                                       xhci::xhci_endpoint* ep,
                                       xhci::endpoint_async_state& state,
                                       const xhci::xhci_transfer_completion_trb_t* event);
    void _mark_async_endpoints_for_device_disconnecting(xhci::xhci_device* device);
    void _clear_async_endpoint_state(xhci::xhci_device* device,
                                     xhci::xhci_endpoint* ep,
                                     usb::transfer_status status);
    void _clear_async_endpoints_for_device(xhci::xhci_device* device,
                                           usb::transfer_status status);

    // Hub event processing (called from HCD task context)
    void _process_hub_events();
    void _setup_hub_device(xhci::xhci_device* hub_device, uint8_t hub_port, uint8_t speed);
    void _teardown_hub_device(xhci::xhci_device* hub_device, uint8_t hub_port);

    // Device setup and management
    void _setup_device(uint8_t port_index);
    void _teardown_device(uint8_t port_index);
    void _configure_device(xhci::xhci_device* device, const usb::usb_device_descriptor& desc);
    void _configure_ctrl_ep_input_context(xhci::xhci_device* device, uint16_t max_packet_size);
    xhci::xhci_endpoint* _create_endpoint(xhci::xhci_device* device, const usb::usb_endpoint_descriptor* desc);
    void _configure_endpoint_context(xhci::xhci_device* device, xhci::xhci_endpoint* ep);
    int32_t _configure_endpoints(xhci::xhci_device* device);
    int32_t _set_configuration(xhci::xhci_device* device, uint8_t config_value);
    int32_t _address_device(xhci::xhci_device* device, bool bsr);
    uint16_t _initial_max_packet_size(uint8_t speed);

    // Slot and endpoint management commands
    int32_t _disable_slot(uint8_t slot_id);
    int32_t _reset_endpoint(xhci::xhci_device* device, uint8_t dci);
    int32_t _stop_endpoint(xhci::xhci_device* device, uint8_t dci);
    int32_t _set_tr_dequeue_ptr(xhci::xhci_device* device, uint8_t dci,
                                uintptr_t new_dequeue_phys, uint8_t dcs);
    int32_t _recover_stalled_control_endpoint(xhci::xhci_device* device);

    // Transfer event completion dispatch
    void _complete_endpoint_transfer(sync::spinlock& lock, sync::wait_queue& wq,
                                     xhci::xhci_transfer_completion_trb_t& result_out,
                                     bool* completed_out,
                                     const xhci::xhci_transfer_completion_trb_t* event);

    // Control transfers
    int32_t _send_control_transfer(xhci::xhci_device* device,
                                   xhci::xhci_device_request_packet& request,
                                   void* buffer, uint32_t length);
    int32_t _get_device_descriptor(xhci::xhci_device* device, void* out, uint16_t length);
    int32_t _get_configuration_descriptor(xhci::xhci_device* device,
                                           usb::usb_configuration_descriptor* out,
                                           uint8_t config_index = 0);

    // Normal (interrupt/bulk) transfers
    int32_t _submit_normal_transfer(xhci::xhci_device* device, xhci::xhci_endpoint* ep,
                                    void* buffer, uint32_t length);
};
}

#endif // STELLUX_DRIVERS_USB_XHCI_XHCI_H