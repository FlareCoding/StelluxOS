#include <modules/usb/xhci/xhci.h>
#include <serial/serial.h>
#include <dynpriv/dynpriv.h>
#include <time/time.h>

namespace modules {
bool xhci_driver_module::s_singleton_initialized = false;

xhci_driver_module::xhci_driver_module() : pci_module_base("xhci_driver_module") {}

bool xhci_driver_module::init() {
    if (s_singleton_initialized) {
        serial::printf("[XHCI] Another instance of the controller driver is already running\n");
        return true;
    }
    s_singleton_initialized = true;

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

    return true;
}

bool xhci_driver_module::start() {
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
    size_t context_entry_size = m_64byte_context_size ? 64 : 32;
    size_t dcbaa_size = context_entry_size * (m_max_device_slots + 1);

    m_dcbaa = reinterpret_cast<uint64_t*>(
        alloc_xhci_memory(dcbaa_size, XHCI_DEVICE_CONTEXT_ALIGNMENT, XHCI_DEVICE_CONTEXT_BOUNDARY)
    );

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
    }

    // Set DCBAA pointer in the operational registers
    m_op_regs->dcbaap = xhci_get_physical_addr(m_dcbaa);
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
            portsc.csc = 1;
            regset.write_portsc_reg(portsc);
            return true;
        }
    }

    return false; 
}
} // namespace modules
