#include "drivers/usb/xhci/xhci.h"
#include "drivers/usb/xhci/xhci_common.h"
#include "drivers/usb/xhci/xhci_ext_cap.h"
#include "common/logging.h"
#include "mm/heap.h"
#include "clock/clock.h"
#include "hw/delay.h"
#include "dynpriv/dynpriv.h"

using namespace drivers::xhci;

namespace drivers {

int32_t xhci_hcd::attach() {
    // Enable PCI memory space + bus mastering before any MMIO access
    RUN_ELEVATED({
        dev().enable();
        dev().enable_bus_mastering();
    });

    int32_t rc = map_bar(0, m_xhc_base, paging::PAGE_USER);
    if (rc != 0) {
        log::error("xhci: failed to map BAR0: %d", rc);
        return rc;
    }

    m_xhc_bar_size = dev().get_bar(0).size;

    m_xhc_cap_regs = reinterpret_cast<volatile xhci_capability_registers*>(m_xhc_base);

    // Validate that critical MMIO offsets fall within the mapped BAR
    uint32_t caplength = m_xhc_cap_regs->caplength;
    uint32_t dboff = m_xhc_cap_regs->dboff;
    uint32_t rtsoff = m_xhc_cap_regs->rtsoff;

    if (caplength >= m_xhc_bar_size || dboff >= m_xhc_bar_size || rtsoff >= m_xhc_bar_size) {
        log::error("xhci: MMIO offsets exceed BAR size (caplength=0x%x, dboff=0x%x, rtsoff=0x%x, bar_size=0x%lx)",
                   caplength, dboff, rtsoff, m_xhc_bar_size);
        return -1;
    }

    m_xhc_op_regs = reinterpret_cast<volatile xhci_operational_registers*>(m_xhc_base + caplength);
    m_doorbells = reinterpret_cast<volatile uint32_t*>(m_xhc_base + dboff);

    m_hc_params.max_device_slots = XHCI_MAX_DEVICE_SLOTS(m_xhc_cap_regs);
    m_hc_params.max_interrupters = XHCI_MAX_INTERRUPTERS(m_xhc_cap_regs);
    m_hc_params.max_ports = XHCI_MAX_PORTS(m_xhc_cap_regs);
    m_hc_params.ist = XHCI_IST(m_xhc_cap_regs);
    m_hc_params.erst_max = XHCI_ERST_MAX(m_xhc_cap_regs);
    m_hc_params.max_scratchpad_buffers = XHCI_MAX_SCRATCHPAD_BUFFERS(m_xhc_cap_regs);
    m_hc_params.extended_capabilities_offset = XHCI_XECP(m_xhc_cap_regs) * sizeof(uint32_t);
    m_hc_params.ac64 = XHCI_AC64(m_xhc_cap_regs);
    m_hc_params.csz = XHCI_CSZ(m_xhc_cap_regs);
    m_hc_params.ppc = XHCI_PPC(m_xhc_cap_regs);

    _parse_extended_capabilities();

    int32_t reset_rc = _reset_host_controller();
    if (reset_rc != 0) {
        log::error("xhci: host controller reset failed");
        return reset_rc;
    }

    int32_t op_rc = _configure_operational_registers();
    if (op_rc != 0) {
        log::error("xhci: failed to configure operational registers");
        return op_rc;
    }

    int32_t rt_rc = _configure_runtime_registers();
    if (rt_rc != 0) {
        log::error("xhci: failed to configure runtime registers");
        return rt_rc;
    }

    // Setup MSI/MSI-X interrupts
    int32_t msi_rc = -1;
    if (dev().has_capability(pci::CAP_MSIX)) {
        msi_rc = setup_msix(1);
    } else if (dev().has_capability(pci::CAP_MSI)) {
        msi_rc = setup_msi(1);
    }

    if (msi_rc != 0) {
        log::warn("xhci: MSI/MSI-X setup failed, interrupts unavailable");
    }

    return drivers::OK;
}

int32_t xhci_hcd::detach() {
    // Stop the controller (halt + disable interrupts)
    if (m_xhc_op_regs) {
        _stop_host_controller();
    }

    // Destroy event ring
    if (m_event_ring) {
        m_event_ring->destroy();
        heap::ufree_delete(m_event_ring);
        m_event_ring = nullptr;
    }

    // Destroy command ring and clear CRCR
    if (m_cmd_ring) {
        if (m_xhc_op_regs) {
            m_xhc_op_regs->crcr = 0;
        }
        m_cmd_ring->destroy();
        heap::ufree_delete(m_cmd_ring);
        m_cmd_ring = nullptr;
    }

    // Free scratchpad buffers
    if (m_scratchpad.count > 0) {
        for (uint16_t i = 0; i < m_scratchpad.count; i++) {
            if (m_scratchpad.page_vaddrs && m_scratchpad.page_vaddrs[i]) {
                free_xhci_memory(m_scratchpad.page_vaddrs[i]);
            }
        }
        if (m_scratchpad.page_vaddrs) {
            heap::ufree(m_scratchpad.page_vaddrs);
            m_scratchpad.page_vaddrs = nullptr;
        }
        if (m_scratchpad.table) {
            free_xhci_memory(m_scratchpad.table);
            m_scratchpad.table = nullptr;
        }
        m_scratchpad.count = 0;
    }

    // Clear DCBAAP and free DCBAA
    if (m_xhc_op_regs) {
        m_xhc_op_regs->dcbaap = 0;
    }
    if (m_dcbaa) {
        free_xhci_memory(m_dcbaa);
        m_dcbaa = nullptr;
    }

    // Base class tears down MSI and unmaps BARs
    return pci_driver::detach();
}

void xhci_hcd::run() {
    if (_start_host_controller() != 0) {
        log::error("xhci: failed to start controller");
        return;
    }

    while (true) {
        wait_for_event();

        _process_event_ring();
        m_event_ring->finish_processing();
    }
}

__PRIVILEGED_CODE void xhci_hcd::on_interrupt(uint32_t) {
    m_xhc_op_regs->usbsts = XHCI_USBSTS_EINT;
}

void xhci_hcd::_parse_extended_capabilities() {
    if (m_hc_params.extended_capabilities_offset == 0) return;

    if (m_hc_params.extended_capabilities_offset >= m_xhc_bar_size) {
        log::warn("xhci: XECP offset 0x%x exceeds BAR size 0x%lx, skipping extended capabilities",
                  m_hc_params.extended_capabilities_offset, m_xhc_bar_size);
        return;
    }

    volatile uint32_t* cap_ptr = reinterpret_cast<volatile uint32_t*>(
        m_xhc_base + m_hc_params.extended_capabilities_offset);

    while (true) {
        // Validate that this capability entry is within the mapped BAR
        uintptr_t offset = reinterpret_cast<uintptr_t>(cap_ptr) - m_xhc_base;
        if (offset + sizeof(uint32_t) > m_xhc_bar_size) {
            log::warn("xhci: extended capability at offset 0x%lx exceeds BAR size, stopping walk",
                       offset);
            break;
        }

        xhci_extended_capability_entry entry;
        entry.raw = *cap_ptr;

        auto cap_id = static_cast<xhci_extended_capability_code>(entry.id);

        if (cap_id == xhci_extended_capability_code::usb_legacy_support) {
            _request_bios_handoff(cap_ptr);
        } else if (cap_id == xhci_extended_capability_code::supported_protocol) {
            _parse_supported_protocol(cap_ptr);
        }

        if (entry.next == 0) break;
        cap_ptr = XHCI_NEXT_EXT_CAP_PTR(cap_ptr, entry.next);
    }
}

void xhci_hcd::_request_bios_handoff(volatile uint32_t* usblegsup) {
    volatile uint32_t* usblegctlsts = usblegsup + 1;

    // Disable all SMIs to prevent crashes
    uint32_t legctlsts = *usblegctlsts;
    legctlsts &= ~XHCI_LEGACY_SMI_ENABLE_BITS;
    *usblegctlsts = legctlsts;
    delay::us(10000);

    // Set the OS Owned Semaphore in USBLEGSUP
    log::info("xhci: requesting BIOS handoff");
    uint32_t legsup = *usblegsup;
    legsup |= XHCI_LEGACY_OS_OWNED_SEMAPHORE;
    *usblegsup = legsup;
    delay::us(10000);

    // Wait for BIOS to clear the BIOS Owned Semaphore
    constexpr uint32_t HANDOFF_TIMEOUT_US = 5000000; // 5 seconds
    constexpr uint32_t POLL_INTERVAL_US = 1000;       // 1 ms
    uint32_t elapsed = 0;

    while ((*usblegsup & XHCI_LEGACY_BIOS_OWNED_SEMAPHORE) && elapsed < HANDOFF_TIMEOUT_US) {
        delay::us(POLL_INTERVAL_US);
        elapsed += POLL_INTERVAL_US;
    }

    if (*usblegsup & XHCI_LEGACY_BIOS_OWNED_SEMAPHORE) {
        log::warn("xhci: BIOS did not release ownership within %ums, forcing takeover",
                  HANDOFF_TIMEOUT_US / 1000);

        uint32_t forced = *usblegsup;
        forced &= ~XHCI_LEGACY_BIOS_OWNED_SEMAPHORE;
        *usblegsup = forced;
        delay::us(10000);
    } else {
        log::info("xhci: BIOS handoff complete");
    }
}

void xhci_hcd::_parse_supported_protocol(volatile uint32_t* cap_ptr) {
    xhci_usb_supported_protocol_capability proto(cap_ptr);

    // Port offsets in the capability are 1-based, convert to 0-based
    uint8_t first_port = proto.compatible_port_offset - 1;
    uint8_t port_count = proto.compatible_port_count;

    if (proto.major_revision_version == 3) {
        for (uint8_t i = 0; i < port_count; i++) {
            uint8_t port = first_port + i;
            m_usb3_port_map[port / 32] |= (1u << (port % 32));
        }
        log::info("xhci: USB3 ports %u-%u", first_port, first_port + port_count - 1);
    } else if (proto.major_revision_version == 2) {
        log::info("xhci: USB2 ports %u-%u", first_port, first_port + port_count - 1);
    }
}

// xHCI Spec Section 4.2: Host Controller Initialization
int32_t xhci_hcd::_reset_host_controller() {
    // Clear Run/Stop to halt the controller
    uint32_t usbcmd = m_xhc_op_regs->usbcmd;
    usbcmd &= ~XHCI_USBCMD_RUN_STOP;
    m_xhc_op_regs->usbcmd = usbcmd;

    // Wait for HCHalted bit (xHC shall halt within 16ms per spec)
    constexpr uint32_t HALT_TIMEOUT_US = 200000; // 200ms generous timeout
    constexpr uint32_t POLL_US = 1000;           // 1ms poll interval
    uint32_t elapsed = 0;

    while (!(m_xhc_op_regs->usbsts & XHCI_USBSTS_HCH)) {
        if (elapsed >= HALT_TIMEOUT_US) {
            log::error("xhci: controller did not halt within %ums", HALT_TIMEOUT_US / 1000);
            return -1;
        }
        delay::us(POLL_US);
        elapsed += POLL_US;
    }

    // Assert HCRESET
    usbcmd = m_xhc_op_regs->usbcmd;
    usbcmd |= XHCI_USBCMD_HCRESET;
    m_xhc_op_regs->usbcmd = usbcmd;

    // Wait for HCRESET and CNR (Controller Not Ready) to clear
    constexpr uint32_t RESET_TIMEOUT_US = 1000000; // 1 second
    elapsed = 0;

    while (m_xhc_op_regs->usbcmd & XHCI_USBCMD_HCRESET ||
           m_xhc_op_regs->usbsts & XHCI_USBSTS_CNR) {
        if (elapsed >= RESET_TIMEOUT_US) {
            log::error("xhci: controller did not complete reset within %ums",
                       RESET_TIMEOUT_US / 1000);
            return -1;
        }
        delay::us(POLL_US);
        elapsed += POLL_US;
    }

    // Post-reset settling time
    delay::us(50000);

    // Verify operational registers returned to defaults
    if (m_xhc_op_regs->usbcmd != 0 ||
        m_xhc_op_regs->dnctrl != 0 ||
        m_xhc_op_regs->crcr != 0 ||
        m_xhc_op_regs->dcbaap != 0 ||
        m_xhc_op_regs->config != 0) {
        log::error("xhci: operational registers not at defaults after reset");
        return -1;
    }

    return 0;
}

int32_t xhci_hcd::_start_host_controller() {
    // Set Run/Stop, enable interrupter, and host system error reporting
    uint32_t usbcmd = m_xhc_op_regs->usbcmd;
    usbcmd |= XHCI_USBCMD_RUN_STOP;
    usbcmd |= XHCI_USBCMD_INTERRUPTER_ENABLE;
    usbcmd |= XHCI_USBCMD_HOSTSYS_ERROR_ENABLE;
    m_xhc_op_regs->usbcmd = usbcmd;

    // Wait for the controller to transition out of halted state
    constexpr uint32_t START_TIMEOUT_US = 1000000; // 1 second
    constexpr uint32_t POLL_US = 1000;
    uint32_t elapsed = 0;

    while (m_xhc_op_regs->usbsts & XHCI_USBSTS_HCH) {
        if (elapsed >= START_TIMEOUT_US) {
            log::error("xhci: controller did not start within %ums", START_TIMEOUT_US / 1000);
            return -1;
        }
        delay::us(POLL_US);
        elapsed += POLL_US;
    }

    // Verify CNR (Controller Not Ready) bit is clear
    if (m_xhc_op_regs->usbsts & XHCI_USBSTS_CNR) {
        log::error("xhci: controller not ready after start");
        return -1;
    }

    return 0;
}

// xHCI Spec Section 4.2 / 5.4.1: Stopping the Host Controller
int32_t xhci_hcd::_stop_host_controller() {
    // Clear Run/Stop and disable interrupter + host system error
    uint32_t usbcmd = m_xhc_op_regs->usbcmd;
    usbcmd &= ~XHCI_USBCMD_RUN_STOP;
    usbcmd &= ~XHCI_USBCMD_INTERRUPTER_ENABLE;
    usbcmd &= ~XHCI_USBCMD_HOSTSYS_ERROR_ENABLE;
    m_xhc_op_regs->usbcmd = usbcmd;

    // Wait for HCHalted (xHC shall halt within 16ms per spec)
    constexpr uint32_t HALT_TIMEOUT_US = 200000; // 200ms generous timeout
    constexpr uint32_t POLL_US = 1000;
    uint32_t elapsed = 0;

    while (!(m_xhc_op_regs->usbsts & XHCI_USBSTS_HCH)) {
        if (elapsed >= HALT_TIMEOUT_US) {
            log::error("xhci: controller did not halt within %ums", HALT_TIMEOUT_US / 1000);
            return -1;
        }
        delay::us(POLL_US);
        elapsed += POLL_US;
    }

    // Clear any pending EINT so no stale interrupts remain
    m_xhc_op_regs->usbsts = XHCI_USBSTS_EINT;

    // Disable the primary interrupter
    volatile xhci_interrupter_registers* primary = &m_xhc_runtime_regs->ir[0];
    uint32_t iman = primary->iman;
    iman &= ~XHCI_IMAN_INTERRUPT_ENABLE;
    iman |= XHCI_IMAN_INTERRUPT_PENDING; // W1C: clear any pending IP
    primary->iman = iman;

    return 0;
}

int32_t xhci_hcd::_configure_operational_registers() {
    // Validate that the controller supports 4KB pages
    if (!(m_xhc_op_regs->pagesize & 0x1)) {
        log::error("xhci: controller does not support 4KB page size");
        return -1;
    }

    // Enable device notifications
    m_xhc_op_regs->dnctrl = 0xffff;

    // Configure the max device slots
    m_xhc_op_regs->config = static_cast<uint32_t>(m_hc_params.max_device_slots);

    // Allocate the Device Context Base Address Array
    size_t dcbaa_entries = static_cast<size_t>(m_hc_params.max_device_slots) + 1;
    size_t dcbaa_size = dcbaa_entries * sizeof(uint64_t);

    m_dcbaa = static_cast<uint64_t*>(alloc_xhci_memory(dcbaa_size));
    if (!m_dcbaa) {
        log::error("xhci: failed to allocate DCBAA (%lu entries)", dcbaa_entries);
        return -1;
    }

    /*
    // xHci Spec Section 6.1 (page 404)
    If the Max Scratchpad Buffers field of the HCSPARAMS2 register is > '0', then
    the first entry (entry_0) in the DCBAA shall contain a pointer to the Scratchpad
    Buffer Array. If the Max Scratchpad Buffers field of the HCSPARAMS2 register is
    = '0', then the first entry (entry_0) in the DCBAA is reserved and shall be
    cleared to '0' by software.
    */
    if (m_hc_params.max_scratchpad_buffers > 0) {
        uint16_t scratchpad_count = m_hc_params.max_scratchpad_buffers;
        m_scratchpad.count = scratchpad_count;

        // Allocate the scratchpad pointer array (DMA: array of physical addresses)
        m_scratchpad.table = alloc_xhci_memory(
            static_cast<size_t>(scratchpad_count) * sizeof(uint64_t));
        if (!m_scratchpad.table) {
            log::error("xhci: failed to allocate scratchpad pointer array (%u entries)", scratchpad_count);
            goto fail_dcbaa;
        }

        // Allocate tracking array for virtual addresses (heap, for cleanup)
        m_scratchpad.page_vaddrs = static_cast<void**>(
            heap::uzalloc(static_cast<size_t>(scratchpad_count) * sizeof(void*)));
        if (!m_scratchpad.page_vaddrs) {
            log::error("xhci: failed to allocate scratchpad VA tracking array");
            goto fail_scratchpad_table;
        }

        // Allocate each scratchpad page
        auto* scratchpad_phys_array = static_cast<uint64_t*>(m_scratchpad.table);
        for (uint16_t i = 0; i < scratchpad_count; i++) {
            void* page = alloc_xhci_memory(paging::PAGE_SIZE_4KB);
            if (!page) {
                log::error("xhci: failed to allocate scratchpad page %u/%u", i, scratchpad_count);
                // Free previously allocated pages
                for (uint16_t j = 0; j < i; j++) {
                    free_xhci_memory(m_scratchpad.page_vaddrs[j]);
                }
                goto fail_scratchpad_va;
            }
            m_scratchpad.page_vaddrs[i] = page;
            scratchpad_phys_array[i] = xhci_get_physical_addr(page);
        }

        // Point DCBAA[0] to the scratchpad pointer array
        m_dcbaa[0] = xhci_get_physical_addr(m_scratchpad.table);
    }

    // Program DCBAAP only after all allocations succeed
    m_xhc_op_regs->dcbaap = xhci_get_physical_addr(m_dcbaa);

    // Setup the command ring
    m_cmd_ring = heap::ualloc_new<xhci_command_ring>();
    if (!m_cmd_ring) {
        log::error("xhci: failed to allocate command ring object");
        goto fail_dcbaap;
    }

    if (m_cmd_ring->init(XHCI_COMMAND_RING_TRB_COUNT) != 0) {
        log::error("xhci: failed to initialize command ring");
        heap::ufree_delete(m_cmd_ring);
        m_cmd_ring = nullptr;
        goto fail_dcbaap;
    }

    // Write CRCR with the command ring's physical base and cycle bit
    m_xhc_op_regs->crcr = m_cmd_ring->get_physical_base() | m_cmd_ring->get_cycle_bit();

    return 0;

fail_dcbaap:
    m_xhc_op_regs->dcbaap = 0;
fail_scratchpad_va:
    heap::ufree(m_scratchpad.page_vaddrs);
    m_scratchpad.page_vaddrs = nullptr;
fail_scratchpad_table:
    free_xhci_memory(m_scratchpad.table);
    m_scratchpad.table = nullptr;
    m_scratchpad.count = 0;
fail_dcbaa:
    free_xhci_memory(m_dcbaa);
    m_dcbaa = nullptr;
    return -1;
}

int32_t xhci_hcd::_configure_runtime_registers() {
    // Compute the runtime register base from capability registers
    m_xhc_runtime_regs = reinterpret_cast<volatile xhci_runtime_registers*>(
        m_xhc_base + m_xhc_cap_regs->rtsoff);

    // Get the primary interrupter register set
    volatile xhci_interrupter_registers* primary_interrupter = &m_xhc_runtime_regs->ir[0];

    // Enable interrupts on the primary interrupter
    uint32_t iman = primary_interrupter->iman;
    iman |= XHCI_IMAN_INTERRUPT_ENABLE;
    primary_interrupter->iman = iman;

    // Setup the event ring (programs ERSTSZ, ERSTBA, ERDP)
    m_event_ring = heap::ualloc_new<xhci_event_ring>();
    if (!m_event_ring) {
        log::error("xhci: failed to allocate event ring object");
        return -1;
    }

    if (m_event_ring->init(XHCI_EVENT_RING_TRB_COUNT, primary_interrupter) != 0) {
        log::error("xhci: failed to initialize event ring");
        heap::ufree_delete(m_event_ring);
        m_event_ring = nullptr;
        return -1;
    }

    return 0;
}

void xhci_hcd::_process_event_ring() {
    while (m_event_ring->has_unprocessed_events()) {
        xhci_trb_t* trb = m_event_ring->dequeue_trb();
        if (!trb) {
            break;
        }

        switch (trb->trb_type) {
        case XHCI_TRB_TYPE_PORT_STATUS_CHANGE_EVENT: {
            auto* psc = reinterpret_cast<xhci_port_status_change_trb_t*>(trb);
            uint8_t port_id = psc->port_id; // 1-based

            if (port_id < 1 || port_id > m_hc_params.max_ports) {
                log::warn("xhci: port status change for invalid port %u", port_id);
                break;
            }

            xhci_portsc_register portsc;
            portsc.raw = *_portsc(port_id - 1);
            log::info("xhci: port %u status change, PORTSC=0x%x, %s",
                       port_id, portsc.raw, portsc.ccs ? "connected" : "disconnected");
            break;
        }
        case XHCI_TRB_TYPE_CMD_COMPLETION_EVENT: {
            auto* cce = reinterpret_cast<xhci_command_completion_trb_t*>(trb);
            log::debug("xhci: command completion, code=%s, slot=%u",
                        trb_completion_code_to_string(cce->completion_code), cce->slot_id);
            m_cmd_ring->process_event(cce);

            if (m_cmd_state.pending) {
                m_cmd_state.result = *cce;
                m_cmd_state.completed = true;
            }
            break;
        }
        case XHCI_TRB_TYPE_TRANSFER_EVENT: {
            auto* e = reinterpret_cast<xhci_transfer_completion_trb_t*>(trb);
            log::info("xhci: transfer event, code=%s, slot=%u, ep=%u",
                       trb_completion_code_to_string(e->completion_code),
                       e->slot_id, e->endpoint_id);
            break;
        }
        default:
            log::debug("xhci: unhandled event TRB type: %s",
                        trb_type_to_string(static_cast<uint8_t>(trb->trb_type)));
            break;
        }
    }
}

int32_t xhci_hcd::_send_command(xhci_trb_t* trb, xhci_command_completion_trb_t* out) {
    if (!m_cmd_ring->enqueue(trb)) {
        log::error("xhci: command ring full");
        return -1;
    }

    m_cmd_state.pending = true;
    m_cmd_state.completed = false;

    _ring_cmd_doorbell();

    // Fast path: check if completion already arrived
    _process_event_ring();
    m_event_ring->finish_processing();

    // Wait loop: block until command completion or timeout
    constexpr uint64_t CMD_TIMEOUT_MS = 5000;
    uint64_t deadline = clock::now_ns() + CMD_TIMEOUT_MS * 1000000ULL;

    while (!m_cmd_state.completed && clock::now_ns() < deadline) {
        wait_for_event();
        _process_event_ring();
        m_event_ring->finish_processing();
    }

    m_cmd_state.pending = false;

    if (!m_cmd_state.completed) {
        log::error("xhci: command timed out (type=%u)",
                   (trb->control >> XHCI_TRB_TYPE_SHIFT) & 0x3F);
        return -1;
    }

    if (out) {
        *out = m_cmd_state.result;
    }

    if (m_cmd_state.result.completion_code != XHCI_TRB_COMPLETION_CODE_SUCCESS) {
        log::warn("xhci: command failed: %s",
                   trb_completion_code_to_string(m_cmd_state.result.completion_code));
        return -1;
    }

    return 0;
}

// xHCI Spec Section 4.19.5: Port Reset
int32_t xhci_hcd::_reset_port(uint8_t port_index) {
    bool usb3 = _is_usb3_port(port_index);

    xhci_portsc_register portsc;
    portsc.raw = *_portsc(port_index);

    // Power on the port if necessary (spec requires PP=1 before any state change)
    if (portsc.pp == 0) {
        portsc.pp = 1;
        _write_portsc(port_index, portsc.raw);
        delay::us(20000); // 20ms power stabilization
        portsc.raw = *_portsc(port_index);

        if (portsc.pp == 0) {
            log::warn("xhci: port %u failed to power on", port_index);
            return -1;
        }
    }

    // Clear any lingering change bits before initiating the reset
    _ack_portsc_changes(port_index,
        PORTSC_RW1C_BITS); // Clear all change bits

    // Re-read and initiate the port reset
    portsc.raw = *_portsc(port_index);
    if (usb3) {
        portsc.wpr = 1; // Warm port reset for USB 3.x
    } else {
        portsc.pr = 1;  // Standard port reset for USB 2.0
    }
    _write_portsc(port_index, portsc.raw);

    // Wait for reset completion (PRC for USB2, WRC for USB3)
    constexpr uint32_t RESET_TIMEOUT_US = 100000; // 100ms
    constexpr uint32_t POLL_US = 1000;
    uint32_t elapsed = 0;

    while (elapsed < RESET_TIMEOUT_US) {
        portsc.raw = *_portsc(port_index);

        if (usb3 && portsc.wrc) break;
        if (!usb3 && portsc.prc) break;

        delay::us(POLL_US);
        elapsed += POLL_US;
    }

    if (elapsed >= RESET_TIMEOUT_US) {
        log::warn("xhci: port %u reset timed out", port_index);
        return -1;
    }

    delay::us(3000); // Post-reset settling

    // Clear the reset completion and status change bits
    _ack_portsc_changes(port_index,
        (1u << 17) | (1u << 18) | (1u << 19) | (1u << 21)); // CSC|PEC|WRC|PRC

    delay::us(3000);

    // Verify the port is enabled after reset
    portsc.raw = *_portsc(port_index);
    if (portsc.ped == 0) {
        return -1;
    }

    return 0;
}

void xhci_hcd::_ring_doorbell(uint8_t slot_id, uint8_t target) {
    m_doorbells[slot_id] = static_cast<uint32_t>(target);
}

void xhci_hcd::_ring_cmd_doorbell() {
    _ring_doorbell(0, XHCI_DOORBELL_TARGET_COMMAND_RING);
}

volatile uint32_t* xhci_hcd::_portsc(uint8_t port_index) {
    return reinterpret_cast<volatile uint32_t*>(
        reinterpret_cast<uintptr_t>(m_xhc_op_regs) +
        PORT_REGS_BASE_OFFSET + (static_cast<uint32_t>(port_index) * PORT_REGS_STRIDE)
    );
}

void xhci_hcd::_write_portsc(uint8_t port_index, uint32_t value) {
    *_portsc(port_index) = value & ~PORTSC_RW1C_BITS;
}

void xhci_hcd::_ack_portsc_changes(uint8_t port_index, uint32_t change_bits) {
    *_portsc(port_index) = change_bits & PORTSC_RW1C_BITS;
}

bool xhci_hcd::_is_usb3_port(uint8_t port_index) {
    return (m_usb3_port_map[port_index / 32] & (1u << (port_index % 32))) != 0;
}

REGISTER_PCI_DRIVER(xhci_hcd,
    PCI_MATCH(PCI_MATCH_ANY, PCI_MATCH_ANY, 0x0C, 0x03, 0x30),
    PCI_DRIVER_FACTORY(xhci_hcd));

} // namespace drivers
