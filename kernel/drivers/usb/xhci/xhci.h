#ifndef STELLUX_DRIVERS_USB_XHCI_XHCI_H
#define STELLUX_DRIVERS_USB_XHCI_XHCI_H

#include "drivers/pci_driver.h"
#include "drivers/usb/xhci/xhci_regs.h"
#include "drivers/usb/xhci/xhci_rings.h"
#include "drivers/usb/xhci/xhci_device.h"
#include "drivers/usb/xhci/xhci_mem.h"

namespace drivers {
namespace xhci {
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

struct xhci_transfer_state {
    bool pending = false;
    bool completed = false;
    xhci_transfer_completion_trb_t result = {};
};
} // namespace xhci

class xhci_hcd : public pci_driver {
public:
    xhci_hcd(pci::device* dev) : pci_driver("xhci_hcd", dev) {}

    int32_t attach() override;
    int32_t detach() override;
    void run() override;

    /** * @note Privilege: **required** */
    __PRIVILEGED_CODE void on_interrupt(uint32_t vector) override;

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

    // Pending transfer state (single in-flight control/bulk/interrupt transfer)
    xhci::xhci_transfer_state m_xfer_state;

    // Per-port device tracking (indexed by 0-based port_index)
    xhci::xhci_device** m_port_devices = nullptr;

    // USB3 port bitmap (bit N = port N is USB3)
    uint32_t m_usb3_port_map[8] = {};

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

    // Port register helpers (port_index is 0-based)
    volatile uint32_t* _portsc(uint8_t port_index);
    void _write_portsc(uint8_t port_index, uint32_t value);
    void _ack_portsc_changes(uint8_t port_index, uint32_t change_bits);
    bool _is_usb3_port(uint8_t port_index);

private:
    // Device setup and management
    void _setup_device(uint8_t port_index);
    void _configure_device(xhci::xhci_device* device, const usb::usb_device_descriptor& desc);
    void _configure_ctrl_ep_input_context(xhci::xhci_device* device, uint16_t max_packet_size);
    xhci::xhci_endpoint* _create_endpoint(xhci::xhci_device* device, const usb::usb_endpoint_descriptor* desc);
    void _configure_endpoint_context(xhci::xhci_device* device, xhci::xhci_endpoint* ep);
    int32_t _configure_endpoints(xhci::xhci_device* device);
    int32_t _set_configuration(xhci::xhci_device* device, uint8_t config_value);
    int32_t _address_device(xhci::xhci_device* device, bool bsr);
    uint16_t _initial_max_packet_size(uint8_t speed);

    // Control transfers
    int32_t _send_control_transfer(xhci::xhci_device* device,
                                   xhci::xhci_device_request_packet& request,
                                   void* buffer, uint32_t length);
    int32_t _get_device_descriptor(xhci::xhci_device* device, void* out, uint16_t length);
    int32_t _get_configuration_descriptor(xhci::xhci_device* device,
                                           usb::usb_configuration_descriptor* out,
                                           uint8_t config_index = 0);
};
}

#endif // STELLUX_DRIVERS_USB_XHCI_XHCI_H