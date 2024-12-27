#ifndef XCHI_H
#define XCHI_H

#include <modules/pci_module_base.h>
#include <interrupts/irq.h>
#include "xhci_device.h"

/*
// xHci Spec Section 4.2 Host Controller Initialization (page 68)

When the system boots, the host controller is enumerated, assigned a base
address for the xHC register space, and the system software sets the Frame
Length Adjustment (FLADJ) register to a system-specific value.
Refer to Section 4.23.1 Power Wells for a discussion of the effect of Power
Wells on register state after power-on and light resets.
Document Number: 625472, Revision: 1.2b 69
Following are a review of the operations that system software would perform in
order to initialize the xHC using MSI-X as the interrupt mechanism:
    • Initialize the system I/O memory maps, if supported.
    
    • After Chip Hardware Reset6 wait until the Controller Not Ready (CNR) flag
in the USBSTS is ‘0’ before writing any xHC Operational or Runtime
registers.

Note: This text does not imply a specific order for the following operations, however
these operations shall be completed before setting the USBCMD register
Run/Stop (R/S) bit to ‘1’.


    • Program the Max Device Slots Enabled (MaxSlotsEn) field in the CONFIG
register (5.4.7) to enable the device slots that system software is going to
use.

    • Program the Device Context Base Address Array Pointer (DCBAAP) register
(Section 5.4.6 Device Context Base Address Array Pointer Register
(DCBAAP)) with a 64-bit address pointing to where the Device Context
Base Address Array is located.

    • Define the Command Ring Dequeue Pointer by programming the Command
Ring Control Register (Section 5.4.5 Command Ring Control Register
(CRCR)) with a 64-bit address pointing to the starting address of the first
TRB of the Command Ring.

    • Initialize interrupts by:
        o Allocate and initialize the MSI-X Message Table (Section 5.2.8.3 MSI-X
        Table), setting the Message Address and Message Data, and enable the
        vectors. At a minimum, table vector entry 0 shall be initialized and
        enabled. Refer to the PCI specification for more details.
        o Allocate and initialize the MSI-X Pending Bit Array (PBA, Section 5.2.8.4
        MSI-X PBA).
        o Point the Table Offset and PBA Offsets in the MSI-X Capability Structure
        to the MSI-X Message Control Table and Pending Bit Array,
        respectively.
        o Initialize the Message Control register (Section 5.2.8.3 MSI-X Table) of
        the MSI-X Capability Structure.
        o Initialize each active interrupter by:
            ▪ Defining the Event Ring: (refer to Section 4.9.4 Event Ring
            Management for a discussion of Event Ring Management.)

    • Allocate and initialize the Event Ring Segment(s).
Refer to the PCI spec for the initialization and use of MSI or PIN interrupt mechanisms
A Chip Hardware Reset may be either a PCI reset input or an optional power-on reset input to the xHC.

Interrupts are optional. The xHC may be managed by polling Event Rings.
Document Number: 625472, Revision: 1.2b Intel Confidential

    • Allocate the Event Ring Segment Table (ERST) (Section 6.5
Event Ring Segment Table). Initialize ERST table entries to
point to and to define the size (in TRBs) of the respective Event
Ring Segment.

    • Program the Interrupter Event Ring Segment Table Size
(ERSTSZ) register (Section 5.5.2.3.1 Event Ring Segment Table
Size Register (ERSTSZ)) with the number of segments
described by the Event Ring Segment Table.

    • Program the Interrupter Event Ring Dequeue Pointer (ERDP)
register (Section 5.5.2.3.3 Event Ring Dequeue Pointer Register
(ERDP)) with the starting address of the first segment
described by the Event Ring Segment Table.

    • Program the Interrupter Event Ring Segment Table Base
Address (ERSTBA) register (Section 5.5.2.3.2 Event Ring
Segment Table Base Address Register (ERSTBA)) with a 64-bit
address pointer to where the Event Ring Segment Table is
located.

Note that writing the ERSTBA enables the Event Ring. Refer to
Section 4.9.4 Event Ring Management for more information on
the Event Ring registers and their initialization.

    ▪ Defining the interrupts:
        • Enable the MSI-X interrupt mechanism by setting the MSI-X
        Enable flag in the MSI-X Capability Structure Message Control
        register (5.2.8.3).
        • Initializing the Interval field of the Interrupt Moderation register
        (5.5.2.2) with the target interrupt moderation rate.
        • Enable system bus interrupt generation by writing a ‘1’ to the
        Interrupter Enable (INTE) flag of the USBCMD register (5.4.1).
        • Enable the Interrupter by writing a ‘1’ to the Interrupt Enable
        (IE) field of the Interrupter Management register (5.5.2.1).
        • Write the USBCMD (5.4.1) to turn the host controller ON via setting the
        Run/Stop (R/S) bit to ‘1’. This operation allows the xHC to begin accepting
        doorbell references.
*/
namespace modules {

class xhci_driver_module : public pci_module_base {
public:
    xhci_driver_module();
    ~xhci_driver_module() = default;

    // ------------------------------------------------------------------------
    // Lifecycle Hooks (overrides from module_base)
    // ------------------------------------------------------------------------
    
    bool init() override;
    bool start() override;
    bool stop() override;

    // ------------------------------------------------------------------------
    // Command Interface
    // ------------------------------------------------------------------------

    bool on_command(
        uint64_t    command,
        const void* data_in,
        size_t      data_in_size,
        void*       data_out,
        size_t      data_out_size
    );

    void log_usbsts();

    __PRIVILEGED_CODE
    static irqreturn_t xhci_irq_handler(void*, xhci_driver_module* driver);

private:
    static bool s_singleton_initialized;

    uintptr_t m_xhc_base;

    volatile xhci_capability_registers* m_cap_regs;
    volatile xhci_operational_registers* m_op_regs;

    void _parse_capability_registers();
    void _log_capability_registers();

    void _parse_extended_capability_registers();

    void _configure_operational_registers();
    void _log_operational_registers();
    
    uint8_t _get_port_speed(uint8_t port);
    const char* _usb_speed_to_string(uint8_t speed); 

    void _configure_runtime_registers();

    bool _is_usb3_port(uint8_t port_num);
    xhci_port_register_manager _get_port_register_set(uint8_t port_num);

    void _setup_dcbaa();

    // Creates a device context buffer and inserts it into DCBAA
    bool _create_device_context(uint8_t slot_id);

    xhci_command_completion_trb_t* _send_command(xhci_trb_t* trb, uint32_t timeout_ms = 120);
    xhci_transfer_completion_trb_t* _start_control_endpoint_transfer(xhci_transfer_ring* transfer_ring);

    uint16_t _get_max_initial_packet_size(uint8_t port_speed);

private:
    void _process_events();
    void _acknowledge_irq(uint8_t interrupter);

    bool _reset_host_controller();
    void _start_host_controller();

    // Reset a 0-indexed port number
    bool _reset_port(uint8_t port_num);
    uint8_t _enable_device_slot();
    void _configure_device_input_context(xhci_device* dev, uint16_t max_packet_size);

    void _configure_device_endpoint_input_context(xhci_device* dev, xhci_device_endpoint_descriptor* endpoint);

    void _setup_device(uint8_t port);
    bool _address_device(xhci_device* dev, bool bsr);
    bool _configure_endpoint(xhci_device* dev);
    bool _evaluate_context(xhci_device* dev);

    bool _send_usb_request_packet(xhci_device* dev, xhci_device_request_packet& req, void* output_buffer, uint32_t length);

    bool _get_device_descriptor(xhci_device* dev, usb_device_descriptor* desc, uint32_t length);
    bool _get_string_language_descriptor(xhci_device* dev, usb_string_language_descriptor* desc);
    bool _get_string_descriptor(xhci_device* dev, uint8_t descriptor_index, uint8_t langid, usb_string_descriptor* desc);
    bool _get_configuration_descriptor(xhci_device* dev, usb_configuration_descriptor* desc);

    bool _set_device_configuration(xhci_device* dev, uint16_t configuration_value);
    bool _set_protocol(xhci_device* dev, uint8_t interface, uint8_t protocol);

private:
    // CAPLENGTH
    uint8_t m_capability_regs_length;
    
    // HCSPARAMS1
    uint8_t m_max_device_slots;
    uint8_t m_max_interrupters;
    uint8_t m_max_ports;

    // HCSPARAMS2
    uint8_t m_isochronous_scheduling_threshold;
    uint8_t m_erst_max;
    uint8_t m_max_scratchpad_buffers;

    // hccparams1
    bool m_64bit_addressing_capability;
    bool m_bandwidth_negotiation_capability;
    bool m_64byte_context_size;
    bool m_port_power_control;
    bool m_port_indicators;
    bool m_light_reset_capability;
    uint32_t m_extended_capabilities_offset;

    // Linked list of extended capabilities
    kstl::shared_ptr<xhci_extended_capability> m_extended_capabilities_head;

    // Page size supported by host controller
    uint64_t m_hc_page_size;

    // USB3.x-specific ports
    kstl::vector<uint8_t> m_usb3_ports;

    // Device context base address array's virtual address
    uint64_t* m_dcbaa;

    // Since DCBAA stores physical addresses, we want to keep
    // track of the virtual pointers to the output device contexts.
    uint64_t* m_dcbaa_virtual_addresses;

    // Controller class for runtime registers
    kstl::shared_ptr<xhci_runtime_register_manager> m_runtime_register_manager;

    // Main command ring
    kstl::shared_ptr<xhci_command_ring> m_command_ring;

    // Main event ring
    kstl::shared_ptr<xhci_event_ring> m_event_ring;

    // Doorbell register array manager
    kstl::shared_ptr<xhci_doorbell_manager> m_doorbell_manager;

    // Table of connected devices for each device slot
    xhci_device* m_connected_devices[64]; // TO-DO: make this table dynamic

private:
    kstl::vector<xhci_port_status_change_trb_t*> m_port_status_change_events;
    kstl::vector<xhci_command_completion_trb_t*> m_command_completion_events;
    kstl::vector<xhci_transfer_completion_trb_t*> m_transfer_completion_events;

    volatile uint8_t m_command_irq_completed = 0;
    volatile uint8_t m_transfer_irq_completed = 0;
};
} // namespace modules

#endif // XCHI_H
