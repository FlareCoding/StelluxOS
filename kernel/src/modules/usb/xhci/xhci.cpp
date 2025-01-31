#include <modules/usb/xhci/xhci.h>
#include <serial/serial.h>
#include <dynpriv/dynpriv.h>
#include <time/time.h>

#ifdef ARCH_X86_64
#include <arch/x86/cpuid.h>
#define DETECT_QEMU() arch::x86::cpuid_is_running_under_qemu()
#else
#define DETECT_QEMU()
#endif

namespace modules {
bool xhci_driver_module::s_singleton_initialized = false;

xhci_driver_module::xhci_driver_module() : pci_module_base("xhci_driver_module") {}

bool xhci_driver_module::init() {
    if (s_singleton_initialized) {
        serial::printf("[XHCI] Another instance of the controller driver is already running\n");
        return true;
    }
    s_singleton_initialized = true;

    RUN_ELEVATED({ m_qemu_detected = DETECT_QEMU(); });

    serial::printf("[XHCI] Initializing xhci driver\n\n");

    auto pci_bar = m_pci_dev->get_bars()[0];
    RUN_ELEVATED({
        m_xhc_base = xhci_map_mmio(pci_bar.address, pci_bar.size);
    });

    // Parse the read-only capability register space
    _parse_capability_registers();
    _log_capability_registers();

    // Parse the extended capabilities
    _parse_extended_capability_registers();

    // Create a table mapping each slot to a device object
    for (size_t i = 0; i < m_max_device_slots; i++) {
        if (i >= sizeof(m_connected_devices) / sizeof(xhci_device*)) {
            break;
        }

        m_connected_devices[i] = nullptr;
    }

    // Reset the controller
    if (!_reset_host_controller()) {
        return false; 
    }

    // Configure the controller's register spaces
    _configure_operational_registers();
    _configure_runtime_registers();

    // Register the xhci host controller IRQ handler
    if (m_irq_vector != 0) {
        RUN_ELEVATED({
            register_irq_handler(m_irq_vector, reinterpret_cast<irq_handler_t>(xhci_irq_handler), false, static_cast<void*>(this));
        });
        serial::printf("[XHCI] Registered xhci handler at IRQ%i\n\n", m_irq_vector - IRQ0);
    }

    return true;
}

bool xhci_driver_module::start() {
    // At this point the controller is all setup so we can start it
    _start_host_controller();

    // Perform an initial port reset for each port
    for (uint8_t i = 0; i < m_max_ports; i++) {
        _reset_port(i);
    }

    // This code is just a prototype right now and is by no
    // means safe and has critical synchronization issues.
    while (true) {
        msleep(100);
        if (m_port_status_change_events.empty()) {
            continue; 
        }

        for (size_t i = 0; i < m_port_status_change_events.size(); i++) {
            uint8_t port = m_port_status_change_events[i]->port_id;
            uint8_t port_reg_idx = port - 1;

            // For debugging, only test one device on real hardware
            if (!m_qemu_detected && port != 3) {
                continue;
            }
            
            xhci_port_register_manager regman = _get_port_register_set(port_reg_idx);
            xhci_portsc_register portsc;
            regman.read_portsc_reg(portsc);

            if (portsc.ccs) {
                serial::printf("[XHCI] Device connected on port %i - %s\n", port, _usb_speed_to_string(portsc.port_speed));
                _setup_device(port_reg_idx);
                serial::printf("\n");
            } else {
                serial::printf("[XHCI] Device disconnected from port %i\n", port);
            }

            // TO-DO: For debugging purposes only, test only one connected device on real hardware
            if (!m_qemu_detected) {
                break;
            }
        }

        m_port_status_change_events.clear();
    }

    return true;
}

bool xhci_driver_module::stop() {
    return true;
}

bool xhci_driver_module::on_command(
    uint64_t    command,
    const void* data_in,
    size_t      data_in_size,
    void*       data_out,
    size_t      data_out_size
) {
    __unused command;
    __unused data_in;
    __unused data_in_size;
    __unused data_out;
    __unused data_out_size;
    return true;
}

void xhci_driver_module::log_usbsts() {
    uint32_t status = m_op_regs->usbsts;
    serial::printf("===== USBSTS =====\n");
    if (status & XHCI_USBSTS_HCH) serial::printf("    Host Controlled Halted\n");
    if (status & XHCI_USBSTS_HSE) serial::printf("    Host System Error\n");
    if (status & XHCI_USBSTS_EINT) serial::printf("    Event Interrupt\n");
    if (status & XHCI_USBSTS_PCD) serial::printf("    Port Change Detect\n");
    if (status & XHCI_USBSTS_SSS) serial::printf("    Save State Status\n");
    if (status & XHCI_USBSTS_RSS) serial::printf("    Restore State Status\n");
    if (status & XHCI_USBSTS_SRE) serial::printf("    Save/Restore Error\n");
    if (status & XHCI_USBSTS_CNR) serial::printf("    Controller Not Ready\n");
    if (status & XHCI_USBSTS_HCE) serial::printf("    Host Controller Error\n");
    serial::printf("\n");
}

__PRIVILEGED_CODE
irqreturn_t xhci_driver_module::xhci_irq_handler(void*, xhci_driver_module* driver) {
    driver->_process_events();
    driver->_acknowledge_irq(0);

    // Acknowledge the interrupt
    irq_send_eoi();

    // Return indicating that the interrupt was handled successfully
    return IRQ_HANDLED;
}

void xhci_driver_module::_parse_capability_registers() {
    m_cap_regs = reinterpret_cast<volatile xhci_capability_registers*>(m_xhc_base);

    m_capability_regs_length = m_cap_regs->caplength;

    m_max_device_slots = XHCI_MAX_DEVICE_SLOTS(m_cap_regs);
    m_max_interrupters = XHCI_MAX_INTERRUPTERS(m_cap_regs);
    m_max_ports = XHCI_MAX_PORTS(m_cap_regs);

    m_isochronous_scheduling_threshold = XHCI_IST(m_cap_regs);
    m_erst_max = XHCI_ERST_MAX(m_cap_regs);
    m_max_scratchpad_buffers = XHCI_MAX_SCRATCHPAD_BUFFERS(m_cap_regs);

    m_64bit_addressing_capability = XHCI_AC64(m_cap_regs);
    m_bandwidth_negotiation_capability = XHCI_BNC(m_cap_regs);
    m_64byte_context_size = XHCI_CSZ(m_cap_regs);
    m_port_power_control = XHCI_PPC(m_cap_regs);
    m_port_indicators = XHCI_PIND(m_cap_regs);
    m_light_reset_capability = XHCI_LHRC(m_cap_regs);
    m_extended_capabilities_offset = XHCI_XECP(m_cap_regs) * sizeof(uint32_t);

    // Update the base pointer to operational register set
    m_op_regs = reinterpret_cast<volatile xhci_operational_registers*>(m_xhc_base + m_capability_regs_length);

    // Construct a manager class instance for the doorbell register array
    m_doorbell_manager = kstl::shared_ptr<xhci_doorbell_manager>(
        new xhci_doorbell_manager(m_xhc_base + m_cap_regs->dboff)
    );

    // Construct a controller class instance for the runtime register set
    uint64_t runtime_register_base = m_xhc_base + m_cap_regs->rtsoff;
    m_runtime_register_manager = kstl::shared_ptr<xhci_runtime_register_manager>(
        new xhci_runtime_register_manager(runtime_register_base, m_max_interrupters)
    );
}

void xhci_driver_module::_log_capability_registers() {
    serial::printf("===== Xhci Capability Registers (0x%llx) =====\n", (uint64_t)m_cap_regs);
    serial::printf("    Length                : %i\n", m_capability_regs_length);
    serial::printf("    Max Device Slots      : %i\n", m_max_device_slots);
    serial::printf("    Max Interrupters      : %i\n", m_max_interrupters);
    serial::printf("    Max Ports             : %i\n", m_max_ports);
    serial::printf("    IST                   : %i\n", m_isochronous_scheduling_threshold);
    serial::printf("    ERST Max Size         : %i\n", m_erst_max);
    serial::printf("    Scratchpad Buffers    : %i\n", m_max_scratchpad_buffers);
    serial::printf("    64-bit Addressing     : %s\n", m_64bit_addressing_capability ? "yes" : "no");
    serial::printf("    Bandwidth Negotiation : %i\n", m_bandwidth_negotiation_capability);
    serial::printf("    64-byte Context Size  : %s\n", m_64byte_context_size ? "yes" : "no");
    serial::printf("    Port Power Control    : %i\n", m_port_power_control);
    serial::printf("    Port Indicators       : %i\n", m_port_indicators);
    serial::printf("    Light Reset Available : %i\n", m_light_reset_capability);
    serial::printf("\n");
}

void xhci_driver_module::_parse_extended_capability_registers() {
    volatile uint32_t* head_cap_ptr = reinterpret_cast<volatile uint32_t*>(
        m_xhc_base + m_extended_capabilities_offset
    );

    m_extended_capabilities_head = kstl::shared_ptr<xhci_extended_capability>(
        new xhci_extended_capability(head_cap_ptr)
    );

    auto node = m_extended_capabilities_head;
    while (node.get()) {
        if (node->id() == xhci_extended_capability_code::supported_protocol) {
            xhci_usb_supported_protocol_capability cap(node->base());
            // Make the ports zero-based
            uint8_t first_port = cap.compatible_port_offset - 1;
            uint8_t last_port = first_port + cap.compatible_port_count - 1;

            if (cap.major_revision_version == 3) {
                for (uint8_t port = first_port; port <= last_port; port++) {
                    m_usb3_ports.push_back(port);
                }
            }
        }
        node = node->next();
    }
}

void xhci_driver_module::_configure_operational_registers() {
    // Establish host controller's supported page size in bytes
    m_hc_page_size = static_cast<uint64_t>(m_op_regs->pagesize & 0xffff) << 12;
    
    // Enable device notifications 
    m_op_regs->dnctrl = 0xffff;

    // Configure the usbconfig field
    m_op_regs->config = static_cast<uint32_t>(m_max_device_slots);

    // Setup device context base address array and scratchpad buffers
    _setup_dcbaa();

    // Setup the command ring and write CRCR
    m_command_ring = kstl::shared_ptr<xhci_command_ring>(
        new xhci_command_ring(XHCI_COMMAND_RING_TRB_COUNT)
    );
    m_op_regs->crcr = m_command_ring->get_physical_base() | m_command_ring->get_cycle_bit();
}

void xhci_driver_module::_log_operational_registers() {
    serial::printf("===== Xhci Operational Registers (0x%llx) =====\n", (uint64_t)m_op_regs);
    serial::printf("    usbcmd     : 0x%x\n", m_op_regs->usbcmd);
    serial::printf("    usbsts     : 0x%x\n", m_op_regs->usbsts);
    serial::printf("    pagesize   : 0x%x\n", m_op_regs->pagesize);
    serial::printf("    dnctrl     : 0x%x\n", m_op_regs->dnctrl);
    serial::printf("    crcr       : 0x%llx\n", m_op_regs->crcr);
    serial::printf("    dcbaap     : 0x%llx\n", m_op_regs->dcbaap);
    serial::printf("    config     : 0x%x\n", m_op_regs->config);
    serial::printf("\n");
}

uint8_t xhci_driver_module::_get_port_speed(uint8_t port) {
    auto port_register_set = _get_port_register_set(port);
    xhci_portsc_register portsc;
    port_register_set.read_portsc_reg(portsc);

    return static_cast<uint8_t>(portsc.port_speed);
}

const char* xhci_driver_module::_usb_speed_to_string(uint8_t speed) {
    static const char* speed_string[7] = {
        "Invalid",
        "Full Speed (12 MB/s - USB2.0)",
        "Low Speed (1.5 Mb/s - USB 2.0)",
        "High Speed (480 Mb/s - USB 2.0)",
        "Super Speed (5 Gb/s - USB3.0)",
        "Super Speed Plus (10 Gb/s - USB 3.1)",
        "Undefined"
    };

    return speed_string[speed];
}

void xhci_driver_module::_configure_runtime_registers() {
    // Get the primary interrupter registers
    auto interrupter_regs = m_runtime_register_manager->get_interrupter_registers(0);
    if (!interrupter_regs) {
        serial::printf("[*] Failed to retrieve interrupter register set when setting up the event ring!");
        return;
    }

    // Enable interrupts
    interrupter_regs->iman |= XHCI_IMAN_INTERRUPT_ENABLE;

    // Setup the event ring and write to interrupter
    // registers to set ERSTSZ, ERSDP, and ERSTBA.
    m_event_ring = kstl::shared_ptr<xhci_event_ring>(
        new xhci_event_ring(XHCI_EVENT_RING_TRB_COUNT, interrupter_regs)
    );

    // Clear any pending interrupts for primary interrupter
    _acknowledge_irq(0);
}

bool xhci_driver_module::_is_usb3_port(uint8_t port_num) {
    for (size_t i = 0; i < m_usb3_ports.size(); ++i) {
        if (m_usb3_ports[i] == port_num) {
            return true;
        }
    }

    return false;
}

xhci_port_register_manager xhci_driver_module::_get_port_register_set(uint8_t port_num) {
    uint64_t base = reinterpret_cast<uint64_t>(m_op_regs) + (0x400 + (0x10 * port_num));
    return xhci_port_register_manager(base);
}

void xhci_driver_module::_setup_dcbaa() {
    size_t dcbaa_size = sizeof(uintptr_t) * (m_max_device_slots + 1);

    m_dcbaa = reinterpret_cast<uint64_t*>(
        alloc_xhci_memory(dcbaa_size, XHCI_DEVICE_CONTEXT_ALIGNMENT, XHCI_DEVICE_CONTEXT_BOUNDARY)
    );

    m_dcbaa_virtual_addresses = new uint64_t[m_max_device_slots + 1];

    /*
    // xHci Spec Section 6.1 (page 404)

    If the Max Scratchpad Buffers field of the HCSPARAMS2 register is > ‘0’, then
    the first entry (entry_0) in the DCBAA shall contain a pointer to the Scratchpad
    Buffer Array. If the Max Scratchpad Buffers field of the HCSPARAMS2 register is
    = ‘0’, then the first entry (entry_0) in the DCBAA is reserved and shall be
    cleared to ‘0’ by software.
    */

    // Initialize scratchpad buffer array if needed
    if (m_max_scratchpad_buffers > 0) {
        uint64_t* scratchpad_array = reinterpret_cast<uint64_t*>(
            alloc_xhci_memory(
                m_max_scratchpad_buffers * sizeof(uint64_t),
                XHCI_DEVICE_CONTEXT_ALIGNMENT,
                XHCI_DEVICE_CONTEXT_BOUNDARY
            )
        );
        
        // Create scratchpad pages
        for (uint8_t i = 0; i < m_max_scratchpad_buffers; i++) {
            void* scratchpad = alloc_xhci_memory(PAGE_SIZE, XHCI_SCRATCHPAD_BUFFERS_ALIGNMENT, XHCI_SCRATCHPAD_BUFFERS_BOUNDARY);
            uint64_t scratchpadPhysicalBase = xhci_get_physical_addr(scratchpad);

            scratchpad_array[i] = scratchpadPhysicalBase;
        }

        uint64_t scratchpad_array_physical_base = xhci_get_physical_addr(scratchpad_array);

        // Set the first slot in the DCBAA to point to the scratchpad array
        m_dcbaa[0] = scratchpad_array_physical_base;

        m_dcbaa_virtual_addresses[0] = reinterpret_cast<uint64_t>(scratchpad_array);
    }

    // Set DCBAA pointer in the operational registers
    m_op_regs->dcbaap = xhci_get_physical_addr(m_dcbaa);
}

uint16_t xhci_driver_module::_get_max_initial_packet_size(uint8_t port_speed) {
    // Calculate initial max packet size for the set device command
    uint16_t initial_max_packet_size = 0;
    switch (port_speed) {
    case XHCI_USB_SPEED_LOW_SPEED: initial_max_packet_size = 8; break;

    case XHCI_USB_SPEED_FULL_SPEED:
    case XHCI_USB_SPEED_HIGH_SPEED: initial_max_packet_size = 64; break;

    case XHCI_USB_SPEED_SUPER_SPEED:
    case XHCI_USB_SPEED_SUPER_SPEED_PLUS:
    default: initial_max_packet_size = 512; break;
    }

    return initial_max_packet_size;
}

bool xhci_driver_module::_create_device_context(uint8_t slot_id) {
    // Determine the size of the device context
    // based on the capability register parameters.
    uint64_t device_context_size = m_64byte_context_size ? sizeof(xhci_device_context64) : sizeof(xhci_device_context32);

    // Allocate a memory block for the device context
    void* ctx = alloc_xhci_memory(
        device_context_size,
        XHCI_DEVICE_CONTEXT_ALIGNMENT,
        XHCI_DEVICE_CONTEXT_BOUNDARY
    );

    if (!ctx) {
        serial::printf("[XHCI] CRITICAL FAILURE: Failed to allocate memory for a device context\n");
        return false;
    }

    // Insert the device context's physical address
    // into the Device Context Base Addres Array (DCBAA).
    m_dcbaa[slot_id] = xhci_get_physical_addr(ctx);

    // Store the virtual address as well
    m_dcbaa_virtual_addresses[slot_id] = reinterpret_cast<uint64_t>(ctx);
    
    return true;
}

xhci_command_completion_trb_t* xhci_driver_module::_send_command(xhci_trb_t* trb, uint32_t timeout_ms) {
    // Enqueue the TRB
    m_command_ring->enqueue(trb);

    // Ring the command doorbell
    m_doorbell_manager->ring_command_doorbell();

    // Let the host controller process the command
    uint64_t sleep_passed = 0;
    while (!m_command_irq_completed) {
        usleep(10);
        sleep_passed += 10;

        if (sleep_passed > timeout_ms * 1000) {
            break;
        }
    }

    xhci_command_completion_trb_t* completion_trb =
        m_command_completion_events.size() ? m_command_completion_events[0] : nullptr;

    // Reset the irq flag and clear out the command completion event queue
    m_command_completion_events.clear();
    m_command_irq_completed = 0;

    if (!completion_trb) {
        serial::printf("[*] Failed to find completion TRB for command %i\n", trb->trb_type);
        return nullptr;
    }

    if (completion_trb->completion_code != XHCI_TRB_COMPLETION_CODE_SUCCESS) {
        serial::printf("[*] Command TRB failed with error: %s\n", trb_completion_code_to_string(completion_trb->completion_code));
        return nullptr;
    }

    return completion_trb;
}

xhci_transfer_completion_trb_t* xhci_driver_module::_start_control_endpoint_transfer(xhci_transfer_ring* transfer_ring) {
    // Ring the endpoint's doorbell
    m_doorbell_manager->ring_control_endpoint_doorbell(transfer_ring->get_doorbell_id());

    // Let the host controller process the command
    const uint64_t timeout_ms = 400; 
    uint64_t sleep_passed = 0;
    while (!m_transfer_irq_completed) {
        usleep(10);
        sleep_passed += 10;

        if (sleep_passed > timeout_ms * 1000) {
            break;
        }
    }

    xhci_transfer_completion_trb_t* completion_trb =
        m_transfer_completion_events.size() ? m_transfer_completion_events[0] : nullptr;

    // Reset the irq flag and clear out the command completion event queue
    m_transfer_completion_events.clear();
    m_transfer_irq_completed = 0;

    if (!completion_trb) {
        serial::printf("[XHCI] Failed to find transfer completion TRB\n");
        return nullptr;
    }

    if (completion_trb->completion_code != XHCI_TRB_COMPLETION_CODE_SUCCESS) {
        serial::printf("[XHCI] Transfer TRB failed with error: %s\n", trb_completion_code_to_string(completion_trb->completion_code));
        return nullptr;
    }

    return completion_trb;
}

void xhci_driver_module::_process_events() {
    // Poll the event ring for the command completion event
    kstl::vector<xhci_trb_t*> events;
    if (m_event_ring->has_unprocessed_events()) {
        m_event_ring->dequeue_events(events);
    }

    uint8_t port_change_event_status = 0;
    uint8_t command_completion_status = 0;
    uint8_t transfer_completion_status = 0;

    for (size_t i = 0; i < events.size(); i++) {
        xhci_trb_t* event = events[i];
        switch (event->trb_type) {
        case XHCI_TRB_TYPE_PORT_STATUS_CHANGE_EVENT: {
            port_change_event_status = 1;
            m_port_status_change_events.push_back((xhci_port_status_change_trb_t*)event);
            break;
        }
        case XHCI_TRB_TYPE_CMD_COMPLETION_EVENT: {
            command_completion_status = 1;
            m_command_completion_events.push_back((xhci_command_completion_trb_t*)event);
            break;
        }
        case XHCI_TRB_TYPE_TRANSFER_EVENT: {
            transfer_completion_status = 1;
            auto transfer_event = (xhci_transfer_completion_trb_t*)event;
            m_transfer_completion_events.push_back(transfer_event);

            auto device = m_connected_devices[transfer_event->slot_id];
            if (!device) {
                break;
            }

            serial::printf("Device packet received!\n");
            // if (device->usb_device_driver) {
            //     device->usb_device_driver->handle_event(transfer_event);
            // }

            break;
        }
        default: break;
        }
    }

    m_command_irq_completed = command_completion_status;
    m_transfer_irq_completed = transfer_completion_status;
}

void xhci_driver_module::_acknowledge_irq(uint8_t interrupter) {
    // Get the interrupter registers
    xhci_interrupter_registers* interrupter_regs =
        m_runtime_register_manager->get_interrupter_registers(interrupter);

    // Read the current value of IMAN
    uint32_t iman = interrupter_regs->iman;

    // Set the IP bit to '1' to clear it, preserve other bits including IE
    uint32_t iman_write = iman | XHCI_IMAN_INTERRUPT_PENDING;

    // Write back to IMAN
    interrupter_regs->iman = iman_write;

    // Clear the EINT bit in USBSTS by writing '1' to it
    m_op_regs->usbsts = XHCI_USBSTS_EINT;
}

bool xhci_driver_module::_reset_host_controller() {
    // Make sure we clear the Run/Stop bit
    uint32_t usbcmd = m_op_regs->usbcmd;
    usbcmd &= ~XHCI_USBCMD_RUN_STOP;
    m_op_regs->usbcmd = usbcmd;

    // Wait for the HCHalted bit to be set
    uint32_t timeout = 20;
    while (!(m_op_regs->usbsts & XHCI_USBSTS_HCH)) {
        if (--timeout == 0) {
            serial::printf("XHCI HC did not halt within %ims\n", timeout);
            return false;
        }

        msleep(1);
    }

    // Set the HC Reset bit
    usbcmd = m_op_regs->usbcmd;
    usbcmd |= XHCI_USBCMD_HCRESET;
    m_op_regs->usbcmd = usbcmd;

    // Wait for this bit and CNR bit to clear
    timeout = 100;
    while (
        m_op_regs->usbcmd & XHCI_USBCMD_HCRESET ||
        m_op_regs->usbsts & XHCI_USBSTS_CNR
    ) {
        if (--timeout == 0) {
            serial::printf("XHCI HC did not reset within %ims\n", timeout);
            return false;
        }

        msleep(1);
    }

    msleep(50);

    // Check the defaults of the operational registers
    if (m_op_regs->usbcmd != 0)
        return false;

    if (m_op_regs->dnctrl != 0)
        return false;

    if (m_op_regs->crcr != 0)
        return false;

    if (m_op_regs->dcbaap != 0)
        return false;

    if (m_op_regs->config != 0)
        return false;

    return true;
}

void xhci_driver_module::_start_host_controller() {
    uint32_t usbcmd = m_op_regs->usbcmd;
    usbcmd |= XHCI_USBCMD_RUN_STOP;
    usbcmd |= XHCI_USBCMD_INTERRUPTER_ENABLE;
    usbcmd |= XHCI_USBCMD_HOSTSYS_ERROR_ENABLE;

    m_op_regs->usbcmd = usbcmd;

    // Make sure the controller's HCH flag is cleared
    while (m_op_regs->usbsts & XHCI_USBSTS_HCH) {
        msleep(16);
    }
}

bool xhci_driver_module::_reset_port(uint8_t port_num) {
    xhci_port_register_manager regset = _get_port_register_set(port_num);
    xhci_portsc_register portsc;
    regset.read_portsc_reg(portsc);

    bool is_usb3_port = _is_usb3_port(port_num);

    if (portsc.pp == 0) {
        portsc.pp = 1;
        regset.write_portsc_reg(portsc);
        msleep(20);
        regset.read_portsc_reg(portsc);

        if (portsc.pp == 0) {
            serial::printf("Port %i: Bad Reset\n", port_num);
            return false;
        }
    }

    // Clear connect status change bit by writing a '1' to it
    portsc.csc = 1;
    portsc.pec = 1;
    regset.write_portsc_reg(portsc);

    // Write to the appropriate reset bit
    if (is_usb3_port) {
        portsc.wpr = 1;
    } else {
        portsc.pr = 1;
    }
    portsc.ped = 0;
    regset.write_portsc_reg(portsc);

    int timeout = 100;
    while (timeout) {
        regset.read_portsc_reg(portsc);

        // Detect port reset change bit to be set
        if (is_usb3_port && portsc.wrc) {
            break;
        } else if (!is_usb3_port && portsc.prc) {
            break;
        }

        timeout--;
        msleep(1);
    }

    if (timeout > 0) {
        msleep(3);
        regset.read_portsc_reg(portsc);

        // Check for the port enable/disable bit
        // to be set and indicate 'enabled' state.
        if (portsc.ped) {
            // Clear connect status change bit by writing a '1' to it
            portsc.ped = 0;
            portsc.csc = 1;
            portsc.pec = 1;
            portsc.prc = 1;
            regset.write_portsc_reg(portsc);
            return true;
        }
    }

    return false; 
}

uint8_t xhci_driver_module::_enable_device_slot() {
    xhci_trb_t enable_slot_trb = XHCI_CONSTRUCT_CMD_TRB(XHCI_TRB_TYPE_ENABLE_SLOT_CMD);
    auto completion_trb = _send_command(&enable_slot_trb);

    if (!completion_trb) {
        return 0;
    }

    return completion_trb->slot_id;
}

void xhci_driver_module::_configure_device_input_context(xhci_device* device, uint16_t max_packet_size) {
    xhci_input_control_context32* input_control_context = device->get_input_control_context(m_64byte_context_size);
    xhci_slot_context32* slot_context = device->get_input_slot_context(m_64byte_context_size);
    xhci_endpoint_context32* control_ep_context = device->get_input_control_ep_context(m_64byte_context_size);

    // Enable slot and control endpoint contexts
    input_control_context->add_flags = (1 << 0) | (1 << 1);
    input_control_context->drop_flags = 0;

    // Configure the slot context
    slot_context->context_entries = 1;
    slot_context->speed = device->speed;
    slot_context->root_hub_port_num = device->port_number;
    slot_context->route_string = 0;
    slot_context->interrupter_target = 0;

    // Configure the control endpoint context
    control_ep_context->endpoint_state = XHCI_ENDPOINT_STATE_DISABLED;
    control_ep_context->endpoint_type = XHCI_ENDPOINT_TYPE_CONTROL;
    control_ep_context->interval = 0;
    control_ep_context->error_count = 3;
    control_ep_context->max_packet_size = max_packet_size;
    control_ep_context->transfer_ring_dequeue_ptr = device->get_control_ep_transfer_ring()->get_physical_dequeue_pointer_base();
    control_ep_context->dcs = device->get_control_ep_transfer_ring()->get_cycle_bit();
    control_ep_context->max_esit_payload_lo = 0;
    control_ep_context->max_esit_payload_hi = 0;
    control_ep_context->average_trb_length = 8;
}

void xhci_driver_module::_configure_device_endpoint_input_context(xhci_device* device, xhci_device_endpoint_descriptor* endpoint) {
    xhci_input_control_context32* input_control_context = device->get_input_control_context(m_64byte_context_size);
    xhci_slot_context32* slot_context = device->get_input_slot_context(m_64byte_context_size);

    // Enable the input control context flags
    input_control_context->add_flags = (1 << endpoint->endpoint_num) | (1 << 0);
    
    // QEMU doesn't like when you add the control endpoint to the
    // `add_flags` after the slot context is in the Addressed state,
    // but real hardware requires all active endpoints to be specified.
    // if (!m_qemu_detected) {
    //     input_control_context->add_flags |= (1 << 1);
    // }

    input_control_context->drop_flags = 0;
    input_control_context->config_value = endpoint->configuration_value;
    
    if (endpoint->endpoint_num > slot_context->context_entries) {
        slot_context->context_entries = endpoint->endpoint_num;
    }

    // Configure the endpoint context
    xhci_endpoint_context32* interrupt_ep_context =
        device->get_input_ep_context(m_64byte_context_size, endpoint->endpoint_num);

    zeromem(interrupt_ep_context, sizeof(xhci_endpoint_context32));
    interrupt_ep_context->endpoint_state = XHCI_ENDPOINT_STATE_DISABLED;
    interrupt_ep_context->endpoint_type = endpoint->endpoint_type;
    interrupt_ep_context->max_packet_size = endpoint->max_packet_size;
    interrupt_ep_context->max_esit_payload_lo = endpoint->max_packet_size;
    interrupt_ep_context->error_count = 3;
    interrupt_ep_context->max_burst_size = 0;
    interrupt_ep_context->average_trb_length = endpoint->max_packet_size;
    interrupt_ep_context->transfer_ring_dequeue_ptr = endpoint->transfer_ring->get_physical_dequeue_pointer_base();
    interrupt_ep_context->dcs = endpoint->transfer_ring->get_cycle_bit();

    /*
    +------------------------------+------------------+------------------+----------------------------+-------------------------------+
    |          Endpoint            |  bInterval Range |    Time Range    |      Time Computation      | Endpoint Context Valid Range  |
    +------------------------------+------------------+------------------+----------------------------+-------------------------------+
    | FS/LS Interrupt              |       1 - 255    |   1 - 255 ms     | bInterval * 1 ms           |              3 - 10           |
    +------------------------------+------------------+------------------+----------------------------+-------------------------------+
    | FS Isoch                     |       1 - 16     |   1 - 32,768 ms  | 2^(bInterval - 1) * 1 ms   |              3 - 18           |
    +------------------------------+------------------+------------------+----------------------------+-------------------------------+
    | SSP, SS or HS Interrupt or   |                  |                  |                            |                               |
    | Isoch                        |       1 - 16     | 125 µs - 4,096 ms| 2^(bInterval - 1) * 125 µs |              0 - 15           |
    +------------------------------+------------------+------------------+----------------------------+-------------------------------+
    */
    if (device->speed == XHCI_USB_SPEED_HIGH_SPEED || device->speed == XHCI_USB_SPEED_SUPER_SPEED) {
        interrupt_ep_context->interval = endpoint->interval - 1;
    } else {
        interrupt_ep_context->interval = endpoint->interval;
        if (endpoint->interval < 3) {
            interrupt_ep_context->interval = 3;
        }
    }

    serial::printf("[XHCI] Slot Context set up for ep_num=%u\n", endpoint->endpoint_num);
    serial::printf("  context_entries: %u\n", slot_context->context_entries);
    serial::printf("  speed: %u\n", slot_context->speed);

    serial::printf("[XHCI] Input Control Context set up for ep_num=%u\n", endpoint->endpoint_num);
    serial::printf("  drop flags : 0x%x\n", input_control_context->drop_flags);
    serial::printf("  add flags  : 0x%x\n", input_control_context->add_flags);
    serial::printf("  config     : %u\n", input_control_context->config_value);
    serial::printf("  interface  : %u\n", input_control_context->interface_number);
    serial::printf("  altsetting : %u\n", input_control_context->alternate_setting);

    serial::printf("[XHCI] Endpoint Context set up for ep_num=%u config=%u:\n",
                   endpoint->endpoint_num, endpoint->configuration_value);
    serial::printf("  ep_state=%u, ep_type=%u, MPS=%u, interval=%u\n",
                   interrupt_ep_context->endpoint_state,
                   interrupt_ep_context->endpoint_type,
                   interrupt_ep_context->max_packet_size,
                   interrupt_ep_context->interval);
    serial::printf("  avg_TRB_len=%u, error_count=%u, max_burst=%u\n",
                   interrupt_ep_context->average_trb_length,
                   interrupt_ep_context->error_count,
                   interrupt_ep_context->max_burst_size);
    serial::printf("  maxESITlo=%u, maxESIThi=%u\n",
                   interrupt_ep_context->max_esit_payload_lo,
                   interrupt_ep_context->max_esit_payload_hi);
    serial::printf("  dequeue_ptr=0x%llx, dcs=%u\n",
                   (unsigned long long)interrupt_ep_context->transfer_ring_dequeue_ptr,
                   (unsigned)interrupt_ep_context->dcs);
}

void xhci_driver_module::_setup_device(uint8_t port) {
    xhci_device* device = new xhci_device();
    device->port_reg_set = port;
    device->port_number = port + 1;
    device->speed = _get_port_speed(port);

    // Calculate the initial max packet size based on device speed
    uint16_t max_packet_size = _get_max_initial_packet_size(device->speed);

    serial::printf("Setting up %s device on port %i (portReg:%i)\n",
        _usb_speed_to_string(device->speed), device->port_number, port);

    device->slot_id = _enable_device_slot();
    if (!device->slot_id) {
        serial::printf("[XHCI] Failed to setup device - failed to enable device slot\n");
        delete device;
        return;
    }

    serial::printf("Device slot id: %i\n", device->slot_id);
    if (!_create_device_context(device->slot_id)) {
        delete device;
        return;
    }

    // Allocate space for a command input context and transfer ring
    device->allocate_input_context(m_64byte_context_size);
    device->allocate_control_ep_transfer_ring();

    // Configure the command input context
    _configure_device_input_context(device, max_packet_size);

    // First address device with BSR=1, essentially blocking the SET_ADDRESS request,
    // but still enables the control endpoint which we can use to get the device descriptor.
    // Some legacy devices require their desciptor to be read before sending them a SET_ADDRESS command.
    if (!_address_device(device, true)) {
        serial::printf("[XHCI] Failed to setup device - failed to set device address\n");
        return;
    }

    usb_device_descriptor* device_descriptor = new usb_device_descriptor();
    if (!_get_device_descriptor(device, device_descriptor, 8)) {
        serial::printf("[XHCI] Failed to get device descriptor\n");
        return;
    }

    // Update the device input context
    _configure_device_input_context(device, device_descriptor->bMaxPacketSize0);

    // If the read max device packet size is different
    // from the initially calculated one, update it.
    if (device_descriptor->bMaxPacketSize0 != max_packet_size) {
        // Update max packet size with the value from the device descriptor
        max_packet_size = device_descriptor->bMaxPacketSize0;

        if (!_evaluate_context(device)) {
            return;
        }
    }

    // Send the address device command again with BSR=0 this time
    _address_device(device, false);

    // Copy the output device context into the device's input context
    device->copy_output_device_context_to_input_device_context(
        m_64byte_context_size,
        reinterpret_cast<void*>(m_dcbaa_virtual_addresses[device->slot_id])
    );

    // Read the full device descriptor
    if (!_get_device_descriptor(device, device_descriptor, device_descriptor->header.bLength)) {
        serial::printf("[XHCI] Failed to get full device descriptor\n");
        return;
    }

    serial::printf("USB Device Descriptor:\n");
    serial::printf("  bcdUSB:            0x%04x\n", device_descriptor->bcdUsb);
    serial::printf("  bDeviceClass:      0x%02x\n", device_descriptor->bDeviceClass);
    serial::printf("  bDeviceSubClass:   0x%02x\n", device_descriptor->bDeviceSubClass);
    serial::printf("  bDeviceProtocol:   0x%02x\n", device_descriptor->bDeviceProtocol);
    serial::printf("  bMaxPacketSize0:   0x%02x\n", device_descriptor->bMaxPacketSize0);
    serial::printf("  idVendor:          0x%04x\n", device_descriptor->idVendor);
    serial::printf("  idProduct:         0x%04x\n", device_descriptor->idProduct);
    serial::printf("  bcdDevice:         0x%04x\n", device_descriptor->bcdDevice);
    serial::printf("  iManufacturer:     0x%02x\n", device_descriptor->iManufacturer);
    serial::printf("  iProduct:          0x%02x\n", device_descriptor->iProduct);
    serial::printf("  iSerialNumber:     0x%02x\n", device_descriptor->iSerialNumber);
    serial::printf("  bNumConfigurations: 0x%02x\n", device_descriptor->bNumConfigurations);

    usb_string_language_descriptor string_language_descriptor;
    if (!_get_string_language_descriptor(device, &string_language_descriptor)) {
        return;
    }

    // Get the language ID
    uint16_t lang_id = string_language_descriptor.lang_ids[0];

    // Get metadata and information about the device
    usb_string_descriptor* product_name = new usb_string_descriptor();
    if (!_get_string_descriptor(device, device_descriptor->iProduct, lang_id, product_name)) {
        return;
    }

    usb_string_descriptor* manufacturer_name = new usb_string_descriptor();
    if (!_get_string_descriptor(device, device_descriptor->iManufacturer, lang_id, manufacturer_name)) {
        return;
    }

    usb_string_descriptor* serial_number_string = new usb_string_descriptor();
    if (!_get_string_descriptor(device, device_descriptor->iSerialNumber, lang_id, serial_number_string)) {
        return;
    }

    char product[255] = { 0 };
    char manufacturer[255] = { 0 };
    char serial_number[255] = { 0 };

    convert_unicode_to_narrow_string(product_name->unicode_string, product);
    convert_unicode_to_narrow_string(manufacturer_name->unicode_string, manufacturer);
    convert_unicode_to_narrow_string(serial_number_string->unicode_string, serial_number);

    usb_configuration_descriptor* configuration_descriptor = new usb_configuration_descriptor();
    if (!_get_configuration_descriptor(device, configuration_descriptor)) {
        return;
    }

    serial::printf("---- USB Device Info ----\n");
    serial::printf("  Product Name    : %s\n", product);
    serial::printf("  Manufacturer    : %s\n", manufacturer);
    serial::printf("  Serial Number   : %s\n", serial_number);
    serial::printf("  Configuration   :\n");
    serial::printf("      wTotalLength        - %i\n", configuration_descriptor->wTotalLength);
    serial::printf("      bNumInterfaces      - %i\n", configuration_descriptor->bNumInterfaces);
    serial::printf("      bConfigurationValue - %i\n", configuration_descriptor->bConfigurationValue);
    serial::printf("      iConfiguration      - %i\n", configuration_descriptor->iConfiguration);
    serial::printf("      bmAttributes        - %i\n", configuration_descriptor->bmAttributes);
    serial::printf("      bMaxPower           - %i milliamps\n", configuration_descriptor->bMaxPower * 2);

    device->copy_output_device_context_to_input_device_context(
        m_64byte_context_size,
        reinterpret_cast<void*>(m_dcbaa_virtual_addresses[device->slot_id])
    );

    // Set device configuration
    if (!_set_device_configuration(device, configuration_descriptor->bConfigurationValue)) {
        return;
    }

    // Set interface
    if (!_set_interface(device, 0, 0)) {
        return;
    }

    uint8_t* buffer = configuration_descriptor->data;
    uint16_t total_length = configuration_descriptor->wTotalLength - configuration_descriptor->header.bLength;
    uint16_t index = 0;

    while (index < total_length) {
        usb_descriptor_header* header = reinterpret_cast<usb_descriptor_header*>(&buffer[index]);

        switch (header->bDescriptorType) {
            case USB_DESCRIPTOR_INTERFACE: {
                if (device->primary_interface != 0) {
                    break;
                }

                usb_interface_descriptor* iface = reinterpret_cast<usb_interface_descriptor*>(header);
                device->primary_interface = iface->bInterfaceNumber;
                device->interface_class = iface->bInterfaceClass;
                device->interface_sub_class = iface->bInterfaceSubClass;
                device->interface_protocol = iface->bInterfaceProtocol;

                serial::printf("    ------ Device Interface ------\n", device->endpoints.size());
                serial::printf("      class             - %i\n", device->interface_class);
                serial::printf("      sub-class         - %i\n", device->interface_sub_class);
                serial::printf("      protocol          - %i\n", device->interface_protocol);
                serial::printf("      bInterfaceNumber  - %i\n", iface->bInterfaceNumber);
                serial::printf("      bAlternateSetting - %i\n", iface->bAlternateSetting);
                break;
            }
            case USB_DESCRIPTOR_HID: {
                // Process HID Descriptor
                // ...
                break;
            }
            case USB_DESCRIPTOR_ENDPOINT: {
                auto device_ep_descriptor = new xhci_device_endpoint_descriptor(
                    device->slot_id,
                    configuration_descriptor->bConfigurationValue,
                    reinterpret_cast<usb_endpoint_descriptor*>(header)
                );
                device->endpoints.push_back(device_ep_descriptor);

                serial::printf("    ------ Endpoint %lli ------\n", device->endpoints.size());
                serial::printf("      bLength           - %i\n", reinterpret_cast<usb_endpoint_descriptor*>(header)->header.bLength);
                serial::printf("      bDescriptorType   - %i\n", reinterpret_cast<usb_endpoint_descriptor*>(header)->header.bDescriptorType);
                serial::printf("      bEndpointAddress  - 0x%x\n", reinterpret_cast<usb_endpoint_descriptor*>(header)->bEndpointAddress);
                serial::printf("      bmAttributes      - %i\n", reinterpret_cast<usb_endpoint_descriptor*>(header)->bmAttributes);
                serial::printf("      wMaxPacketSize    - %i\n", reinterpret_cast<usb_endpoint_descriptor*>(header)->wMaxPacketSize);
                serial::printf("      endpoint number   - %i\n", device_ep_descriptor->endpoint_num);
                serial::printf("      endpoint type     - %i\n", device_ep_descriptor->endpoint_type);
                serial::printf("      maxPacketSize     - %i\n", device_ep_descriptor->max_packet_size);
                serial::printf("      intervalValue     - %i\n", device_ep_descriptor->interval);
                break;
            }
            default: break;
        }

        index += header->bLength;
    }

    serial::printf("\n");

    // For each of the found endpoints send a configure endpoint command
    for (size_t i = 0; i < device->endpoints.size(); i++) {
        device->copy_output_device_context_to_input_device_context(
            m_64byte_context_size,
            reinterpret_cast<void*>(m_dcbaa_virtual_addresses[device->slot_id])
        );

        xhci_device_endpoint_descriptor* endpoint = device->endpoints[i];
        _configure_device_endpoint_input_context(device, endpoint);

        serial::printf("    ------ Configuring Endpoint %lli ------\n", i + 1);
        serial::printf("      endpoint number   - %i\n", endpoint->endpoint_num);
        serial::printf("      endpoint type     - %i\n", endpoint->endpoint_type);
        serial::printf("      maxPacketSize     - %i\n", endpoint->max_packet_size);
        serial::printf("      intervalValue     - %i\n", endpoint->interval);

        if (!_configure_endpoint(device)) {
            return;
        }

        serial::printf("Endpoint configured successfully!\n");
        
        // TO-DO: This is for debugging the mouse on real hardware, have only one endpoint setup
        break;
    }

    // Update device's input context
    device->copy_output_device_context_to_input_device_context(
        m_64byte_context_size,
        reinterpret_cast<void*>(m_dcbaa_virtual_addresses[device->slot_id])
    );

    serial::printf("Slot ctx slot state   : %s\n", xhci_slot_state_to_string(device->get_input_slot_context(m_64byte_context_size)->slot_state));
    serial::printf("Control ep ctx state  : %s\n", xhci_ep_state_to_string(device->get_input_control_ep_context(m_64byte_context_size)->endpoint_state));
    for (size_t i = 0; i < device->endpoints.size(); i++) {
        auto ep = device->get_input_ep_context(m_64byte_context_size, device->endpoints[i]->endpoint_num);
        serial::printf(
            "Endpoint%i ctx      : %s\n",
            device->endpoints[i]->endpoint_num - 2,
            xhci_ep_state_to_string(ep->endpoint_state)
        );
        serial::printf("  type             : %u\n", ep->endpoint_type);
        serial::printf("  max_packet_size  : %u\n", ep->max_packet_size);
        serial::printf("  interval         : %u\n", ep->interval);
    }

    // Detect if the USB device is an HID device
    // if (device->interface_class == 3 && device->interface_sub_class == 1) {
    //     device->usb_device_driver = new xhci_hid_driver(m_doorbell_manager.get(), device);
    // }

    // Register the device in the device table
    m_connected_devices[device->slot_id] = device;

    // If the device has a valid driver, start it
    // if (device->usb_device_driver) {
    //     device->usb_device_driver->start();
    // }

    serial::printf("[XHCI] Device setup complete\n");

    auto endpoint = device->endpoints[0];
    auto& transfer_ring = endpoint->transfer_ring;

    xhci_normal_trb_t normal_trb;
    zeromem(&normal_trb, sizeof(xhci_normal_trb_t));
    normal_trb.trb_type = XHCI_TRB_TYPE_NORMAL;
    normal_trb.data_buffer_physical_base = xhci_get_physical_addr(endpoint->data_buffer);
    normal_trb.trb_transfer_length = endpoint->max_packet_size;
    normal_trb.ioc = 1;

    transfer_ring->enqueue(reinterpret_cast<xhci_trb_t*>(&normal_trb));
    m_doorbell_manager->ring_doorbell(device->slot_id, endpoint->endpoint_num);
}

bool xhci_driver_module::_address_device(xhci_device* device, bool bsr) {
    // Construct the Address Device TRB
    xhci_address_device_command_trb_t address_device_trb;
    zeromem(&address_device_trb, sizeof(xhci_address_device_command_trb_t));
    address_device_trb.trb_type                     = XHCI_TRB_TYPE_ADDRESS_DEVICE_CMD;
    address_device_trb.input_context_physical_base  = device->get_input_context_physical_base();
    address_device_trb.bsr                          = bsr ? 1 : 0;
    address_device_trb.slot_id                      = device->slot_id;

    // Send the Address Device command
    xhci_command_completion_trb_t* completion_trb =
        _send_command(reinterpret_cast<xhci_trb_t*>(&address_device_trb), 200);

    if (!completion_trb) {
        serial::printf("[*] Failed to address device with BSR=%i\n", (int)bsr);
        return false;
    }

    return true;
}

bool xhci_driver_module::_configure_endpoint(xhci_device* device) {
    xhci_configure_endpoint_command_trb_t configure_ep_trb;
    zeromem(&configure_ep_trb, sizeof(xhci_configure_endpoint_command_trb_t));
    configure_ep_trb.trb_type = XHCI_TRB_TYPE_CONFIGURE_ENDPOINT_CMD;
    configure_ep_trb.input_context_physical_base = device->get_input_context_physical_base();
    configure_ep_trb.slot_id = device->slot_id;

    // Send the Configure Endpoint command
    xhci_command_completion_trb_t* completion_trb =
        _send_command(reinterpret_cast<xhci_trb_t*>(&configure_ep_trb), 200);

    if (!completion_trb) {
        serial::printf("[*] Failed to send Configure Endpoint command\n");
        return false;
    }

    // Check the completion code
    if (completion_trb->completion_code != XHCI_TRB_COMPLETION_CODE_SUCCESS) {
        serial::printf("[XHCI] Configure Endpoint command failed with completion code: %s\n",
                    trb_completion_code_to_string(completion_trb->completion_code));
        return false;
    }

    return true;
}

bool xhci_driver_module::_evaluate_context(xhci_device* device) {
    // Construct the Evaluate Context Command TRB
    xhci_evaluate_context_command_trb_t evaluate_context_trb;
    zeromem(&evaluate_context_trb, sizeof(xhci_evaluate_context_command_trb_t));
    evaluate_context_trb.trb_type = XHCI_TRB_TYPE_EVALUATE_CONTEXT_CMD;
    evaluate_context_trb.input_context_physical_base = device->get_input_context_physical_base();
    evaluate_context_trb.slot_id = device->slot_id;

    // Send the Evaluate Context command
    xhci_command_completion_trb_t* completion_trb =
        _send_command(reinterpret_cast<xhci_trb_t*>(&evaluate_context_trb), 200);

    if (!completion_trb) {
        serial::printf("[*] Failed to send Evaluate Context command\n");
        return false;
    }

    // Check the completion code
    if (completion_trb->completion_code != XHCI_TRB_COMPLETION_CODE_SUCCESS) {
        serial::printf("[*] Evaluate Context command failed with completion code: %s\n",
                    trb_completion_code_to_string(completion_trb->completion_code));
        return false;
    }

    // Optionally, perform a sanity check similar to _address_device
    if (m_64byte_context_size) {
        xhci_device_context64* device_context =
            reinterpret_cast<xhci_device_context64*>(m_dcbaa_virtual_addresses[device->slot_id]);

        serial::printf("    DeviceContext[slotId=%i] address:0x%llx slot_state:%s ep_state:%s max_packet_size:%i\n",
            device->slot_id, device_context->slot_context.device_address,
            xhci_slot_state_to_string(device_context->slot_context.slot_state),
            xhci_ep_state_to_string(device_context->control_ep_context.endpoint_state),
            device_context->control_ep_context.max_packet_size
        );
    } else {
        xhci_device_context32* device_context =
            reinterpret_cast<xhci_device_context32*>(m_dcbaa_virtual_addresses[device->slot_id]);

        serial::printf("    DeviceContext[slotId=%i] address:0x%llx slot_state:%s ep_state:%s max_packet_size:%i\n",
            device->slot_id, device_context->slot_context.device_address,
            xhci_slot_state_to_string(device_context->slot_context.slot_state),
            xhci_ep_state_to_string(device_context->control_ep_context.endpoint_state),
            device_context->control_ep_context.max_packet_size
        );
    }

    return true;
}

bool xhci_driver_module::_send_usb_request_packet(xhci_device* device, xhci_device_request_packet& req, void* output_buffer, uint32_t length) {
    xhci_transfer_ring* transfer_ring = device->get_control_ep_transfer_ring();

    uint32_t* transfer_status_buffer = reinterpret_cast<uint32_t*>(alloc_xhci_memory(sizeof(uint32_t), 16, 16));
    uint8_t* descriptor_buffer = reinterpret_cast<uint8_t*>(alloc_xhci_memory(256, 256, 256));
    
    xhci_setup_stage_trb_t setup_stage;
    zeromem(&setup_stage, sizeof(xhci_trb_t));
    setup_stage.trb_type = XHCI_TRB_TYPE_SETUP_STAGE;
    setup_stage.request_packet = req;
    setup_stage.trb_transfer_length = 8;
    setup_stage.interrupter_target = 0;
    setup_stage.trt = 3;
    setup_stage.idt = 1;
    setup_stage.ioc = 0;

    xhci_data_stage_trb_t data_stage;
    zeromem(&data_stage, sizeof(xhci_trb_t));
    data_stage.trb_type = XHCI_TRB_TYPE_DATA_STAGE;
    data_stage.data_buffer = xhci_get_physical_addr(descriptor_buffer);
    data_stage.trb_transfer_length = length;
    data_stage.td_size = 0;
    data_stage.interrupter_target = 0;
    data_stage.dir = 1;
    data_stage.chain = 1;
    data_stage.ioc = 0;
    data_stage.idt = 0;

    // Clear the status buffer
    *transfer_status_buffer = 0;

    xhci_event_data_trb_t event_data_first;
    zeromem(&event_data_first, sizeof(xhci_trb_t));
    event_data_first.trb_type = XHCI_TRB_TYPE_EVENT_DATA;
    event_data_first.data = xhci_get_physical_addr(transfer_status_buffer);
    event_data_first.interrupter_target = 0;
    event_data_first.chain = 0;
    event_data_first.ioc = 1;

    transfer_ring->enqueue(reinterpret_cast<xhci_trb_t*>(&setup_stage));
    transfer_ring->enqueue(reinterpret_cast<xhci_trb_t*>(&data_stage));
    transfer_ring->enqueue(reinterpret_cast<xhci_trb_t*>(&event_data_first));
    
    // QEMU doesn't quite handle SETUP/DATA/STATUS transactions correctly.
    // It will wait for the STATUS TRB before it completes the transfer.
    // Technically, you need to check for a good transfer before you send the
    //  STATUS TRB.  However, since QEMU doesn't update the status until after
    //  the STATUS TRB, waiting here will not complete a successful transfer.
    //  Bochs and real hardware handles this correctly, however QEMU does not.
    // If you are using QEMU, do not ring the doorbell here.  Ring the doorbell
    //  *after* you place the STATUS TRB on the ring.
    // (See bug report: https://bugs.launchpad.net/qemu/+bug/1859378 )
    if (!m_qemu_detected) {
        auto completion_trb = _start_control_endpoint_transfer(transfer_ring);
        if (!completion_trb) {
            free_xhci_memory(transfer_status_buffer);
            free_xhci_memory(descriptor_buffer);
            return false;
        }

        // serial::printf(
        //     "Transfer Status: %s  Length: %i\n",
        //     trb_completion_code_to_string(completion_trb->completion_code),
        //     completion_trb->transfer_length
        // );
    }

    xhci_status_stage_trb_t status_stage;
    zeromem(&status_stage, sizeof(xhci_trb_t));
    status_stage.trb_type = XHCI_TRB_TYPE_STATUS_STAGE;
    status_stage.interrupter_target = 0;
    status_stage.chain = 1;
    status_stage.ioc = 0;
    status_stage.dir = 0;

    // Clear the status buffer
    *transfer_status_buffer = 0;

    xhci_event_data_trb_t event_data_second;
    zeromem(&event_data_second, sizeof(xhci_trb_t));
    event_data_second.trb_type = XHCI_TRB_TYPE_EVENT_DATA;
    event_data_second.ioc = 1;

    transfer_ring->enqueue(reinterpret_cast<xhci_trb_t*>(&status_stage));
    transfer_ring->enqueue(reinterpret_cast<xhci_trb_t*>(&event_data_second));

    auto completion_trb = _start_control_endpoint_transfer(transfer_ring);
    if (!completion_trb) {
        free_xhci_memory(transfer_status_buffer);
        free_xhci_memory(descriptor_buffer);
        return false;
    }

    // serial::printf(
    //     "Transfer Status: %s  Length: %i\n",
    //     trb_completion_code_to_string(completion_trb->completion_code),
    //     completion_trb->transfer_length
    // );

    // Copy the descriptor into the requested user buffer location
    memcpy(output_buffer, descriptor_buffer, length);

    free_xhci_memory(transfer_status_buffer);
    free_xhci_memory(descriptor_buffer);

    return true;
}

bool xhci_driver_module::_send_usb_no_data_request_packet(xhci_device* dev, xhci_device_request_packet& req) {
    // 1) Get the control endpoint's transfer ring
    xhci_transfer_ring* transfer_ring = dev->get_control_ep_transfer_ring();
    if (!transfer_ring) {
        serial::printf("No control transfer ring allocated.\n");
        return false;
    }

    // 2) Create the Setup Stage TRB
    xhci_setup_stage_trb_t setup_stage;
    zeromem(&setup_stage, sizeof(setup_stage));
    setup_stage.trb_type = XHCI_TRB_TYPE_SETUP_STAGE;

    // Copy the request packet (bmRequestType, bRequest, wValue, wIndex, wLength)
    setup_stage.request_packet = req;

    // TRT=0 => no data stage
    // If (bmRequestType & 0x80) and wLength>0 => TRT=3 (IN data)
    // If (!(bmRequestType & 0x80)) and wLength>0 => TRT=2 (OUT data)
    setup_stage.trt = 0;  // No data stage
    setup_stage.idt = 1;  // Immediate Data
    setup_stage.ioc = 0;  // We'll complete on the Status Stage or Event Data
    setup_stage.trb_transfer_length = 8; // Setup packet length is always 8

    // 3) Create the Status Stage TRB
    xhci_status_stage_trb_t status_stage;
    zeromem(&status_stage, sizeof(status_stage));
    status_stage.trb_type = XHCI_TRB_TYPE_STATUS_STAGE;

    // For a host->device (or no-data) control transfer, the status stage is an IN handshake => dir=1
    status_stage.dir = 1;      // 1 = IN handshake
    status_stage.chain = 0;    // or 1 if you want to chain to an Event Data TRB
    status_stage.ioc = 1;      // Interrupt on completion

    // 4) Enqueue the TRBs
    transfer_ring->enqueue(reinterpret_cast<xhci_trb_t*>(&setup_stage));
    transfer_ring->enqueue(reinterpret_cast<xhci_trb_t*>(&status_stage));

    // 5) Ring the doorbell and wait for completion
    auto completion_trb = _start_control_endpoint_transfer(transfer_ring);
    if (!completion_trb) {
        serial::printf("[XHCI] No-Data request: Timed out or failed.\n");
        return false;
    }

    return true;
}

bool xhci_driver_module::_get_device_descriptor(xhci_device* device, usb_device_descriptor* desc, uint32_t length) {
    xhci_device_request_packet req;
    req.bRequestType = 0x80; // Device to Host, Standard, Device
    req.bRequest = 6; // GET_DESCRIPTOR
    req.wValue = USB_DESCRIPTOR_REQUEST(USB_DESCRIPTOR_DEVICE, 0);
    req.wIndex = 0;
    req.wLength = length;

    return _send_usb_request_packet(device, req, desc, length);
}

bool xhci_driver_module::_get_string_language_descriptor(xhci_device* device, usb_string_language_descriptor* desc) {
    xhci_device_request_packet req;
    req.bRequestType = 0x80;
    req.bRequest = 6; // GET_DESCRIPTOR
    req.wValue = USB_DESCRIPTOR_REQUEST(USB_DESCRIPTOR_STRING, 0);
    req.wIndex = 0;
    req.wLength = sizeof(usb_descriptor_header);

    // First read just the header in order to get the total descriptor size
    if (!_send_usb_request_packet(device, req, desc, sizeof(usb_descriptor_header))) {
        serial::printf("[XHCI] Failed to read device string language descriptor header\n");
        return false;
    }

    // Read the entire descriptor
    req.wLength = desc->header.bLength;

    if (!_send_usb_request_packet(device, req, desc, desc->header.bLength)) {
        serial::printf("[XHCI] Failed to read device string language descriptor\n");
        return false;
    }

    return true;
}

bool xhci_driver_module::_get_string_descriptor(
    xhci_device* device,
    uint8_t descriptor_index,
    uint8_t langid,
    usb_string_descriptor* desc
) {
    xhci_device_request_packet req;
    req.bRequestType = 0x80; // Device to Host, Standard, Device
    req.bRequest = 6; // GET_DESCRIPTOR
    req.wValue = USB_DESCRIPTOR_REQUEST(USB_DESCRIPTOR_STRING, descriptor_index);
    req.wIndex = langid;
    req.wLength = sizeof(usb_descriptor_header);

    // First read just the header in order to get the total descriptor size
    if (!_send_usb_request_packet(device, req, desc, sizeof(usb_descriptor_header))) {
        serial::printf("[XHCI] Failed to read device string descriptor header\n");
        return false;
    }

    // Read the entire descriptor
    req.wLength = desc->header.bLength;

    if (!_send_usb_request_packet(device, req, desc, desc->header.bLength)) {
        serial::printf("[XHCI] Failed to read device string descriptor\n");
        return false;
    }

    return true;
}

bool xhci_driver_module::_get_configuration_descriptor(xhci_device* device, usb_configuration_descriptor* desc) {
    xhci_device_request_packet req;
    req.bRequestType = 0x80; // Device to Host, Standard, Device
    req.bRequest = 6; // GET_DESCRIPTOR
    req.wValue = USB_DESCRIPTOR_REQUEST(USB_DESCRIPTOR_CONFIGURATION, 0);
    req.wIndex = 0;
    req.wLength = sizeof(usb_descriptor_header);

    // First read just the header in order to get the total descriptor size
    if (!_send_usb_request_packet(device, req, desc, sizeof(usb_descriptor_header))) {
        serial::printf("[XHCI] Failed to read device configuration descriptor header\n");
        return false;
    }

    // Read the entire descriptor
    req.wLength = desc->header.bLength;

    if (!_send_usb_request_packet(device, req, desc, desc->header.bLength)) {
        serial::printf("[XHCI] Failed to read device configuration descriptor\n");
        return false;
    }

    // Read the additional bytes for the interface descriptors as well
    req.wLength = desc->wTotalLength;

    if (!_send_usb_request_packet(device, req, desc, desc->wTotalLength)) {
        serial::printf("[XHCI] Failed to read device configuration descriptor with interface descriptors\n");
        return false;
    }

    return true;
}

bool xhci_driver_module::_set_device_configuration(xhci_device* device, uint16_t configuration_value) {
    // Prepare the setup packet
    xhci_device_request_packet setup_packet;
    zeromem(&setup_packet, sizeof(xhci_device_request_packet));
    setup_packet.bRequestType = 0x00; // Host to Device, Standard, Device
    setup_packet.bRequest = 9;        // SET_CONFIGURATION
    setup_packet.wValue = configuration_value;
    setup_packet.wIndex = 0;
    setup_packet.wLength = 0;

    // Perform the control transfer
    if (!_send_usb_no_data_request_packet(device, setup_packet)) {
        serial::printf("[XHCI] Failed to set device configuration\n");
        return false;
    }

    return true;
}

bool xhci_driver_module::_set_protocol(xhci_device* device, uint8_t interface, uint8_t protocol) {
    xhci_device_request_packet setup_packet;
    zeromem(&setup_packet, sizeof(xhci_device_request_packet));
    setup_packet.bRequestType = 0x21; // Host to Device, Class, Interface
    setup_packet.bRequest = 0x0B;     // SET_PROTOCOL
    setup_packet.wValue = protocol;
    setup_packet.wIndex = interface;
    setup_packet.wLength = 0;

    if (!_send_usb_no_data_request_packet(device, setup_packet)) {
        serial::printf("[XHCI] Failed to set device protocol\n");
        return false;
    }

    return true;
}

bool xhci_driver_module::_set_interface(xhci_device* device, uint8_t interface_number, uint8_t alternate_setting) {
    // Build the Setup Packet for the standard SET_INTERFACE request
    xhci_device_request_packet req;
    zeromem(&req, sizeof(req));
    req.bRequestType = 0x01;  // Host->Device, Standard, Interface
    req.bRequest     = 0x0B;  // SET_INTERFACE
    req.wValue       = alternate_setting;
    req.wIndex       = interface_number;
    req.wLength      = 0;

    // Because it's zero-length and Host->Device, use your no-data request function
    if (!_send_usb_no_data_request_packet(device, req)) {
        serial::printf("[XHCI] Failed to set interface to altSetting=%u on interface=%u\n",
                       alternate_setting, interface_number);
        return false;
    }

    return true;
}
} // namespace modules
