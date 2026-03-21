#include "drivers/usb/xhci/xhci.h"
#include "drivers/usb/xhci/xhci_common.h"
#include "drivers/usb/xhci/xhci_ext_cap.h"
#include "drivers/usb/core/usb_core.h"
#include "drivers/usb/hub/hub_descriptors.h"
#include "common/logging.h"
#include "common/string.h"
#include "mm/heap.h"
#include "clock/clock.h"
#include "hw/delay.h"
#include "hw/barrier.h"
#include "sync/wait_queue.h"
#include "dynpriv/dynpriv.h"
#include "sched/sched.h"

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
        log::error("xhci: failed to map BAR: %d", rc);
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

    // Allocate per-port device tracking array
    m_port_devices = static_cast<xhci_device**>(
        heap::uzalloc(static_cast<size_t>(m_hc_params.max_ports) * sizeof(xhci_device*)));
    if (!m_port_devices) {
        log::error("xhci: failed to allocate port device tracking array");
        return -1;
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

    // Destroy per-port devices
    if (m_port_devices) {
        for (uint8_t i = 0; i < m_hc_params.max_ports; i++) {
            _teardown_device(i);
        }
        heap::ufree(m_port_devices);
        m_port_devices = nullptr;
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
            xhci_write64(&m_xhc_op_regs->crcr, 0);
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
        xhci_write64(&m_xhc_op_regs->dcbaap, 0);
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

    // Let ports stabilize after the controller transitions to running.
    // Real hardware (especially VL805 behind PCIe) needs time for link
    // training and device detection before port status is meaningful.
    delay::us(100000);

    // Scan for devices connected before the controller started.
    // QEMU and some hardware platforms don't generate PSC events
    // for boot-attached devices.
    _scan_ports();

    while (true) {
        wait_for_event();

        _process_event_ring();
        m_event_ring->finish_processing();

        _process_hub_events();
    }
}

__PRIVILEGED_CODE void xhci_hcd::on_interrupt(uint32_t) {
    // Clear USBSTS.EINT first, then IMAN.IP (RW1C). Clearing IP allows the
    // 0->1 transition needed to generate the next MSI. Without this, real xHCs
    // (including VL805) won't generate subsequent interrupts.
    m_xhc_op_regs->usbsts = XHCI_USBSTS_EINT;
    volatile xhci::xhci_interrupter_registers* ir = &m_xhc_runtime_regs->ir[0];
    ir->iman = XHCI_IMAN_INTERRUPT_PENDING | XHCI_IMAN_INTERRUPT_ENABLE;
    (void)ir->iman; // read-back flushes posted PCIe write
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
        xhci_read64(&m_xhc_op_regs->crcr) != 0 ||
        xhci_read64(&m_xhc_op_regs->dcbaap) != 0 ||
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

    // Ensure all DCBAA/scratchpad writes (Normal NC) are visible before DCBAAP (Device)
    barrier::dma_write();
    xhci_write64(&m_xhc_op_regs->dcbaap, xhci_get_physical_addr(m_dcbaa));

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

    // Ensure command ring link TRB (Normal NC) is visible before CRCR (Device)
    barrier::dma_write();
    xhci_write64(&m_xhc_op_regs->crcr,
                 m_cmd_ring->get_physical_base() | m_cmd_ring->get_cycle_bit());

    return 0;

fail_dcbaap:
    xhci_write64(&m_xhc_op_regs->dcbaap, 0);
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
    barrier::dma_read();

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

            if (portsc.csc && portsc.ccs) {
                _setup_device(port_id - 1);
            } else if (portsc.csc && !portsc.ccs) {
                log::info("xhci: device disconnected from port %u", port_id);
                _teardown_device(port_id - 1);
                _ack_portsc_changes(port_id - 1, PORTSC_RW1C_BITS);
            }
            break;
        }
        case XHCI_TRB_TYPE_CMD_COMPLETION_EVENT: {
            auto* cce = reinterpret_cast<xhci_command_completion_trb_t*>(trb);
            m_cmd_ring->process_event(cce);

            if (m_cmd_state.pending) {
                m_cmd_state.result = *cce;
                m_cmd_state.completed = true;
            }
            break;
        }
        case XHCI_TRB_TYPE_TRANSFER_EVENT: {
            auto* e = reinterpret_cast<xhci_transfer_completion_trb_t*>(trb);
            uint8_t slot = e->slot_id;
            uint8_t ep_id = e->endpoint_id;

            if (slot == 0 || slot > 255 || ep_id == 0 || ep_id > 31) {
                log::warn("xhci: transfer event with invalid slot=%u ep=%u", slot, ep_id);
                break;
            }

            auto* dev = m_slot_devices[slot];
            if (!dev) {
                break;
            }

            if (ep_id == 1) {
                _complete_endpoint_transfer(
                    dev->ctrl_completion_lock(), dev->ctrl_completion_wq(),
                    dev->ctrl_result(), dev->ctrl_completed_ptr(), e);
            } else {
                auto* ep = dev->endpoint(ep_id);
                if (!ep) break;
                _complete_endpoint_transfer(
                    ep->completion_lock(), ep->completion_wq(),
                    ep->result(), ep->completed_ptr(), e);
            }
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

    if (!m_cmd_state.completed) {
        constexpr uint64_t CMD_TIMEOUT_MS = 5000;
        uint64_t deadline = clock::now_ns() + CMD_TIMEOUT_MS * 1000000ULL;

        while (!m_cmd_state.completed && clock::now_ns() < deadline) {
            wait_for_event();
            _process_event_ring();
            m_event_ring->finish_processing();
        }
    }

    m_cmd_state.pending = false;

    if (!m_cmd_state.completed) {
        log::error("xhci: command timed out (type=%u, USBSTS=0x%08x)",
                   (trb->control >> XHCI_TRB_TYPE_SHIFT) & 0x3F,
                   m_xhc_op_regs->usbsts);
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
    constexpr uint32_t RESET_TIMEOUT_US = 500000; // 500ms
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

void xhci_hcd::_scan_ports() {
    for (uint8_t i = 0; i < m_hc_params.max_ports; i++) {
        xhci_portsc_register portsc;
        portsc.raw = *_portsc(i);

        if (portsc.ccs) {
            _setup_device(i);
        }
    }
}

void xhci_hcd::_ring_doorbell(uint8_t slot_id, uint8_t target) {
    barrier::dma_write();
    m_doorbells[slot_id] = static_cast<uint32_t>(target);
    (void)m_doorbells[slot_id]; // read-back flushes posted PCIe write
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
    (void)*_portsc(port_index); // read-back flushes posted PCIe write
}

void xhci_hcd::_ack_portsc_changes(uint8_t port_index, uint32_t change_bits) {
    uint32_t portsc = *_portsc(port_index);
    portsc &= ~PORTSC_RW1C_BITS;                // preserve R/W bits, zero all RW1C
    portsc |= (change_bits & PORTSC_RW1C_BITS); // write-1-to-clear the targeted ones
    *_portsc(port_index) = portsc;
    (void)*_portsc(port_index);                 // flush posted writes
}

bool xhci_hcd::_is_usb3_port(uint8_t port_index) {
    return (m_usb3_port_map[port_index / 32] & (1u << (port_index % 32))) != 0;
}

int32_t xhci_hcd::_disable_slot(uint8_t slot_id) {
    xhci_disable_slot_command_trb_t trb = {};
    trb.trb_type = XHCI_TRB_TYPE_DISABLE_SLOT_CMD;
    trb.slot_id = slot_id;
    return _send_command(reinterpret_cast<xhci_trb_t*>(&trb));
}

void xhci_hcd::_teardown_device(uint8_t port_index) {
    auto* device = m_port_devices[port_index];
    if (!device) return;

    uint8_t slot_id = device->slot_id();

    // If this device is a hub, tear down all downstream devices first
    if (device->is_hub()) {
        for (uint8_t i = 1; i <= device->hub_num_ports(); i++) {
            _teardown_hub_device(device, i);
        }
    }

    // Wake any endpoint waiters so they don't hang (e.g., on disconnect)
    RUN_ELEVATED({
        for (uint8_t i = 2; i <= xhci_device::MAX_ENDPOINTS; i++) {
            auto* ep = device->endpoint(i);
            if (!ep) continue;
            sync::irq_state irq = sync::spin_lock_irqsave(ep->completion_lock());
            ep->set_completed(true);
            sync::spin_unlock_irqrestore(ep->completion_lock(), irq);
            sync::wake_all(ep->completion_wq());
        }

        // Also wake any EP0 waiter
        sync::irq_state irq = sync::spin_lock_irqsave(device->ctrl_completion_lock());
        device->set_ctrl_completed(true);
        sync::spin_unlock_irqrestore(device->ctrl_completion_lock(), irq);
        sync::wake_all(device->ctrl_completion_wq());
    });

    // Notify USB Core to let bound class drivers call disconnect()
    usb::core::device_disconnected(this, device);

    _disable_slot(slot_id); // tolerate failure (device may be gone)

    // Save output_ctx before destroy() clears it
    void* output_ctx = device->output_ctx();
    device->destroy();
    free_xhci_memory(output_ctx);
    m_dcbaa[slot_id] = 0;
    m_port_devices[port_index] = nullptr;
    m_slot_devices[slot_id] = nullptr;
    heap::ufree_delete(device);
}

void xhci_hcd::_complete_endpoint_transfer(
    sync::spinlock& lock, sync::wait_queue& wq,
    xhci_transfer_completion_trb_t& result_out, bool* completed_out,
    const xhci_transfer_completion_trb_t* event
) {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(lock);
        result_out = *event;
        *completed_out = true;
        sync::spin_unlock_irqrestore(lock, irq);
        sync::wake_one(wq);
    });
}

int32_t xhci_hcd::_reset_endpoint(xhci_device* device, uint8_t dci) {
    xhci_reset_endpoint_command_trb_t trb = {};
    trb.trb_type = XHCI_TRB_TYPE_RESET_ENDPOINT_CMD;
    trb.endpoint_id = dci;
    trb.slot_id = device->slot_id();
    return _send_command(reinterpret_cast<xhci_trb_t*>(&trb));
}

int32_t xhci_hcd::_stop_endpoint(xhci_device* device, uint8_t dci) {
    xhci_stop_endpoint_command_trb_t trb = {};
    trb.trb_type = XHCI_TRB_TYPE_STOP_ENDPOINT_CMD;
    trb.endpoint_id = dci;
    trb.slot_id = device->slot_id();
    return _send_command(reinterpret_cast<xhci_trb_t*>(&trb));
}

int32_t xhci_hcd::_set_tr_dequeue_ptr(xhci_device* device, uint8_t dci,
                                       uintptr_t new_dequeue_phys, uint8_t dcs) {
    xhci_set_tr_dequeue_ptr_command_trb_t trb = {};
    trb.trb_type = XHCI_TRB_TYPE_SET_TR_DEQUEUE_PTR_CMD;
    trb.endpoint_id = dci;
    trb.slot_id = device->slot_id();
    trb.new_dequeue_ptr = (new_dequeue_phys & ~static_cast<uintptr_t>(0xF))
                        | (static_cast<uintptr_t>(dcs) & 1);
    return _send_command(reinterpret_cast<xhci_trb_t*>(&trb));
}

void xhci_hcd::_setup_device(uint8_t port_index) {
    uint8_t port_id = port_index + 1; // 1-based for logging and slot context

    // Idempotent: skip if this port already has a device
    if (m_port_devices[port_index]) {
        return;
    }

    // Reset the port
    if (_reset_port(port_index) != 0) {
        log::error("xhci: failed to reset port %u", port_id);
        return;
    }

    // Read port speed after reset
    xhci_portsc_register portsc;
    portsc.raw = *_portsc(port_index);
    uint8_t port_speed = static_cast<uint8_t>(portsc.port_speed);

    // Enable a device slot
    xhci_trb_t enable_slot = XHCI_CONSTRUCT_CMD_TRB(XHCI_TRB_TYPE_ENABLE_SLOT_CMD);
    xhci_command_completion_trb_t completion = {};

    if (_send_command(&enable_slot, &completion) != 0) {
        log::error("xhci: enable slot failed for port %u", port_id);
        return;
    }

    uint8_t slot_id = completion.slot_id;

    // Allocate the output device context
    size_t dev_ctx_size = m_hc_params.csz
        ? sizeof(xhci_device_context64)
        : sizeof(xhci_device_context32);

    void* output_ctx = alloc_xhci_memory(dev_ctx_size);
    if (!output_ctx) {
        log::error("xhci: failed to allocate device context for slot %u", slot_id);
        _disable_slot(slot_id);
        return;
    }

    // Write the physical address into DCBAA[slot_id]
    m_dcbaa[slot_id] = xhci_get_physical_addr(output_ctx);

    // Create and initialize the device object
    auto* device = heap::ualloc_new<xhci_device>();
    if (!device) {
        log::error("xhci: failed to allocate xhci_device for slot %u", slot_id);
        free_xhci_memory(output_ctx);
        m_dcbaa[slot_id] = 0;
        _disable_slot(slot_id);
        return;
    }

    if (device->init(port_id, slot_id, port_speed, m_hc_params.csz) != 0) {
        log::error("xhci: failed to init xhci_device for slot %u", slot_id);
        heap::ufree_delete(device);
        free_xhci_memory(output_ctx);
        m_dcbaa[slot_id] = 0;
        _disable_slot(slot_id);
        return;
    }

    // Root hub device: route_string=0, root_port_id=port_id, no parent
    device->set_root_port_id(port_id);

    device->set_output_ctx(output_ctx);
    m_port_devices[port_index] = device;
    m_slot_devices[slot_id] = device;

    _enumerate_device(device);
}

void xhci_hcd::queue_hub_enumerate(xhci_device* hub_device, uint8_t hub_port, uint8_t speed) {
    xhci::hub_event evt;
    evt.type = xhci::hub_event_type::enumerate;
    evt.hub_slot_id = hub_device->slot_id();
    evt.hub_port = hub_port;
    evt.speed = speed;

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_hub_event_lock);
        uint8_t next_tail = (m_hub_event_tail + 1) % xhci::HUB_EVENT_QUEUE_SIZE;
        if (next_tail != m_hub_event_head) {
            m_hub_events[m_hub_event_tail] = evt;
            m_hub_event_tail = next_tail;
        } else {
            log::error("xhci: hub event queue full");
        }
        sync::spin_unlock_irqrestore(m_hub_event_lock, irq);

        sync::irq_state irq2 = sync::spin_lock_irqsave(m_irq_lock);
        m_event_pending = true;
        sync::spin_unlock_irqrestore(m_irq_lock, irq2);
        sync::wake_one(m_irq_wq);
    });
}

void xhci_hcd::queue_hub_disconnect(xhci_device* hub_device, uint8_t hub_port) {
    xhci::hub_event evt;
    evt.type = xhci::hub_event_type::disconnect;
    evt.hub_slot_id = hub_device->slot_id();
    evt.hub_port = hub_port;
    evt.speed = 0;

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_hub_event_lock);
        uint8_t next_tail = (m_hub_event_tail + 1) % xhci::HUB_EVENT_QUEUE_SIZE;
        if (next_tail != m_hub_event_head) {
            m_hub_events[m_hub_event_tail] = evt;
            m_hub_event_tail = next_tail;
        } else {
            log::error("xhci: hub event queue full");
        }
        sync::spin_unlock_irqrestore(m_hub_event_lock, irq);

        sync::irq_state irq2 = sync::spin_lock_irqsave(m_irq_lock);
        m_event_pending = true;
        sync::spin_unlock_irqrestore(m_irq_lock, irq2);
        sync::wake_one(m_irq_wq);
    });
}

void xhci_hcd::_process_hub_events() {
    while (true) {
        xhci::hub_event evt;
        bool has_event = false;

        RUN_ELEVATED({
            sync::irq_state irq = sync::spin_lock_irqsave(m_hub_event_lock);
            if (m_hub_event_head != m_hub_event_tail) {
                evt = m_hub_events[m_hub_event_head];
                m_hub_event_head = (m_hub_event_head + 1) % xhci::HUB_EVENT_QUEUE_SIZE;
                has_event = true;
            }
            sync::spin_unlock_irqrestore(m_hub_event_lock, irq);
        });

        if (!has_event) break;

        auto* hub_device = m_slot_devices[evt.hub_slot_id];
        if (!hub_device) continue;

        if (evt.type == xhci::hub_event_type::enumerate) {
            _setup_hub_device(hub_device, evt.hub_port, evt.speed);
        } else if (evt.type == xhci::hub_event_type::disconnect) {
            _teardown_hub_device(hub_device, evt.hub_port);
        }
    }
}

void xhci_hcd::_setup_hub_device(xhci_device* hub_device, uint8_t hub_port, uint8_t speed) {
    if (!hub_device || hub_port == 0 || hub_port > xhci_device::MAX_HUB_PORTS) {
        return;
    }

    // Don't re-enumerate if already present
    if (hub_device->hub_child(hub_port)) {
        return;
    }

    // Enable a device slot
    xhci_trb_t enable_slot = XHCI_CONSTRUCT_CMD_TRB(XHCI_TRB_TYPE_ENABLE_SLOT_CMD);
    xhci_command_completion_trb_t completion = {};

    if (_send_command(&enable_slot, &completion) != 0) {
        log::error("xhci: enable slot failed for hub slot %u port %u",
                   hub_device->slot_id(), hub_port);
        return;
    }

    uint8_t slot_id = completion.slot_id;

    // Allocate the output device context
    size_t dev_ctx_size = m_hc_params.csz
        ? sizeof(xhci_device_context64)
        : sizeof(xhci_device_context32);

    void* output_ctx = alloc_xhci_memory(dev_ctx_size);
    if (!output_ctx) {
        log::error("xhci: failed to allocate device context for slot %u", slot_id);
        _disable_slot(slot_id);
        return;
    }

    m_dcbaa[slot_id] = xhci_get_physical_addr(output_ctx);

    auto* device = heap::ualloc_new<xhci_device>();
    if (!device) {
        log::error("xhci: failed to allocate xhci_device for slot %u", slot_id);
        free_xhci_memory(output_ctx);
        m_dcbaa[slot_id] = 0;
        _disable_slot(slot_id);
        return;
    }

    // Use 1-based hub_port as port_id for logging context
    if (device->init(hub_port, slot_id, speed, m_hc_params.csz) != 0) {
        log::error("xhci: failed to init xhci_device for slot %u", slot_id);
        heap::ufree_delete(device);
        free_xhci_memory(output_ctx);
        m_dcbaa[slot_id] = 0;
        _disable_slot(slot_id);
        return;
    }

    // Compute route string: parent's route string with this hub_port in the
    // lowest available nibble. Each tier occupies 4 bits of the route string
    // (USB 3.1 spec Section 8.9, xHCI spec Section 6.2.2).
    uint32_t parent_rs = hub_device->route_string();
    uint32_t rs = parent_rs;
    uint8_t port_nibble = (hub_port > 15) ? 15 : hub_port;
    for (uint8_t shift = 0; shift < 20; shift += 4) {
        if (((rs >> shift) & 0xF) == 0) {
            rs |= (static_cast<uint32_t>(port_nibble) << shift);
            break;
        }
    }

    device->set_route_string(rs);
    device->set_root_port_id(hub_device->root_port_id());

    // Always track the direct parent for teardown/cleanup
    device->set_parent_slot_id(hub_device->slot_id());
    device->set_parent_port_num(hub_port);

    // Propagate TT fields down the chain. For a HS hub these come from the
    // hub descriptor; for non-HS hubs they were already inherited from the
    // nearest HS ancestor. _configure_ctrl_ep_input_context walks up to find
    // the actual HS hub when building the slot context.
    device->set_tt_think_time(hub_device->tt_think_time());
    device->set_mtt(hub_device->mtt());

    device->set_output_ctx(output_ctx);
    hub_device->set_hub_child(hub_port, device);
    m_slot_devices[slot_id] = device;

    log::info("xhci: hub slot %u port %u -> slot %u (route=0x%05x, speed=%u)",
              hub_device->slot_id(), hub_port, slot_id, rs, speed);

    _enumerate_device(device);
}

void xhci_hcd::_teardown_hub_device(xhci_device* hub_device, uint8_t hub_port) {
    if (!hub_device || hub_port == 0 || hub_port > xhci_device::MAX_HUB_PORTS) {
        return;
    }

    auto* device = hub_device->hub_child(hub_port);
    if (!device) return;

    uint8_t slot_id = device->slot_id();

    // If this device is itself a hub, tear down its children first
    if (device->is_hub()) {
        for (uint8_t i = 1; i <= device->hub_num_ports(); i++) {
            _teardown_hub_device(device, i);
        }
    }

    // Wake any endpoint waiters
    RUN_ELEVATED({
        for (uint8_t i = 2; i <= xhci_device::MAX_ENDPOINTS; i++) {
            auto* ep = device->endpoint(i);
            if (!ep) continue;
            sync::irq_state irq = sync::spin_lock_irqsave(ep->completion_lock());
            ep->set_completed(true);
            sync::spin_unlock_irqrestore(ep->completion_lock(), irq);
            sync::wake_all(ep->completion_wq());
        }

        sync::irq_state irq = sync::spin_lock_irqsave(device->ctrl_completion_lock());
        device->set_ctrl_completed(true);
        sync::spin_unlock_irqrestore(device->ctrl_completion_lock(), irq);
        sync::wake_all(device->ctrl_completion_wq());
    });

    usb::core::device_disconnected(this, device);
    _disable_slot(slot_id);

    void* output_ctx = device->output_ctx();
    device->destroy();
    free_xhci_memory(output_ctx);
    m_dcbaa[slot_id] = 0;
    hub_device->set_hub_child(hub_port, nullptr);
    m_slot_devices[slot_id] = nullptr;
    heap::ufree_delete(device);

    log::info("xhci: hub slot %u port %u device disconnected (slot %u)",
              hub_device->slot_id(), hub_port, slot_id);
}

int32_t xhci_hcd::configure_as_hub(xhci_device* device, uint8_t num_ports,
                                    uint8_t tt_think_time, bool mtt) {
    device->set_is_hub(true);
    device->set_hub_num_ports(num_ports);
    device->set_tt_think_time(tt_think_time);
    device->set_mtt(mtt);

    device->sync_input_ctx();

    auto* input_ctrl = device->input_ctrl_ctx();
    auto* slot_ctx = device->input_slot_ctx();

    input_ctrl->add_flags = (1u << 0);
    input_ctrl->drop_flags = 0;

    slot_ctx->hub = 1;
    slot_ctx->port_count = num_ports;
    slot_ctx->mtt = mtt ? 1 : 0;
    if (device->speed() == XHCI_USB_SPEED_HIGH_SPEED) {
        slot_ctx->tt_think_time = tt_think_time;
    }

    // xHCI spec Section 6.2.2.3: if the Hub field was not set before the
    // first Configure Endpoint Command (which already ran in _configure_device),
    // software must use Evaluate Context to update Hub/MTT/Ports/TTT fields.
    xhci_evaluate_context_command_trb_t eval = {};
    eval.trb_type = XHCI_TRB_TYPE_EVALUATE_CONTEXT_CMD;
    eval.input_context_physical_base = device->input_ctx_phys();
    eval.slot_id = device->slot_id();

    int32_t rc = _send_command(reinterpret_cast<xhci_trb_t*>(&eval));
    if (rc != 0) {
        log::error("xhci: failed to evaluate hub context for slot %u", device->slot_id());
    }
    return rc;
}

void xhci_hcd::_enumerate_device(xhci_device* device) {
    uint8_t port_id = device->port_id();
    uint8_t slot_id = device->slot_id();
    uint8_t port_speed = device->speed();

    bool is_root_device = (device->route_string() == 0);
    uint8_t port_index = port_id - 1;

    #define ENUM_FAIL(msg, ...) do { \
        log::error(msg, ##__VA_ARGS__); \
        if (is_root_device) { \
            _teardown_device(port_index); \
        } else { \
            auto* parent = m_slot_devices[device->parent_slot_id()]; \
            if (parent) _teardown_hub_device(parent, device->parent_port_num()); \
        } \
        return; \
    } while (0)

    // Configure the input context for Address Device command
    uint16_t max_packet_size = _initial_max_packet_size(port_speed);
    _configure_ctrl_ep_input_context(device, max_packet_size);

    if (_address_device(device, true) != 0)
        ENUM_FAIL("xhci: address device (BSR=1) failed for slot %u", slot_id);

    usb::usb_device_descriptor desc = {};
    if (_get_device_descriptor(device, &desc, 8) != 0)
        ENUM_FAIL("xhci: failed to read device descriptor for slot %u", slot_id);

    if (desc.bMaxPacketSize0 != max_packet_size) {
        max_packet_size = desc.bMaxPacketSize0;
        _configure_ctrl_ep_input_context(device, max_packet_size);

        xhci_evaluate_context_command_trb_t eval_ctx = {};
        eval_ctx.trb_type = XHCI_TRB_TYPE_EVALUATE_CONTEXT_CMD;
        eval_ctx.input_context_physical_base = device->input_ctx_phys();
        eval_ctx.slot_id = slot_id;

        if (_send_command(reinterpret_cast<xhci_trb_t*>(&eval_ctx)) != 0)
            ENUM_FAIL("xhci: evaluate context failed for slot %u", slot_id);
    }

    if (_address_device(device, false) != 0)
        ENUM_FAIL("xhci: address device (BSR=0) failed for slot %u", slot_id);

    device->sync_input_ctx();

    if (_get_device_descriptor(device, &desc, sizeof(usb::usb_device_descriptor)) != 0)
        ENUM_FAIL("xhci: failed to read full device descriptor for slot %u", slot_id);

    #undef ENUM_FAIL

    log::info("xhci: port %u slot %u: USB %x.%x vid=0x%04x pid=0x%04x mps0=%u configs=%u",
              port_id, slot_id,
              desc.bcdUsb >> 8, (desc.bcdUsb >> 4) & 0xF,
              desc.idVendor, desc.idProduct, desc.bMaxPacketSize0,
              desc.bNumConfigurations);

    _configure_device(device, desc);
}

void xhci_hcd::_configure_device(xhci_device* device, const usb::usb_device_descriptor& desc) {
    uint8_t slot_id = device->slot_id();

    usb::usb_configuration_descriptor config = {};
    if (_get_configuration_descriptor(device, &config) != 0) {
        log::error("xhci: failed to read config descriptor for slot %u", slot_id);
        return;
    }

    log::info("xhci: slot %u config: %u interface(s), totalLength=%u",
              slot_id, config.bNumInterfaces, config.wTotalLength);

    device->sync_input_ctx();

    auto* input_ctrl = device->input_ctrl_ctx();
    input_ctrl->add_flags = (1u << 0);
    input_ctrl->drop_flags = 0;

    // Parse descriptors: track interfaces and associate endpoints
    uint16_t offset = 0;
    uint16_t data_length = config.wTotalLength > 9 ? config.wTotalLength - 9 : 0;
    if (data_length > sizeof(config.data)) {
        data_length = sizeof(config.data);
    }

    xhci_interface_info* current_iface = nullptr;

    while (offset < data_length) {
        auto* hdr = reinterpret_cast<usb::usb_descriptor_header*>(&config.data[offset]);
        if (hdr->bLength == 0) break;

        if (hdr->bDescriptorType == usb::USB_DESCRIPTOR_INTERFACE) {
            if (device->num_interfaces() < xhci_device::MAX_INTERFACES) {
                auto* iface_desc = reinterpret_cast<usb::usb_interface_descriptor*>(hdr);
                uint8_t idx = device->num_interfaces();
                device->set_num_interfaces(idx + 1);
                current_iface = &device->interface_info_mut(idx);
                current_iface->interface_number = iface_desc->bInterfaceNumber;
                current_iface->alternate_setting = iface_desc->bAlternateSetting;
                current_iface->interface_class = iface_desc->bInterfaceClass;
                current_iface->interface_subclass = iface_desc->bInterfaceSubClass;
                current_iface->interface_protocol = iface_desc->bInterfaceProtocol;
                current_iface->num_endpoints = 0;
                log::info("xhci:   interface %u: class=0x%02x subclass=0x%02x protocol=0x%02x",
                          iface_desc->bInterfaceNumber,
                          iface_desc->bInterfaceClass,
                          iface_desc->bInterfaceSubClass,
                          iface_desc->bInterfaceProtocol);
            }
        } else if (hdr->bDescriptorType == usb::USB_DESCRIPTOR_ENDPOINT) {
            auto* ep_desc = reinterpret_cast<usb::usb_endpoint_descriptor*>(hdr);
            auto* ep = _create_endpoint(device, ep_desc);
            if (ep) {
                _configure_endpoint_context(device, ep);
                if (current_iface && current_iface->num_endpoints < 16) {
                    current_iface->endpoint_dcis[current_iface->num_endpoints++] = ep->dci();
                }
            }
        }

        offset += hdr->bLength;
    }

    if (_configure_endpoints(device) != 0) {
        return;
    }

    if (_set_configuration(device, config.bConfigurationValue) != 0) {
        return;
    }

    log::info("xhci: slot %u configured", slot_id);

    // Hand off to USB Core for class driver matching and binding
    usb::core::device_configured(this, device, desc);
}

xhci_endpoint* xhci_hcd::_create_endpoint(xhci_device* device, const usb::usb_endpoint_descriptor* desc) {
    auto* ep = heap::ualloc_new<xhci_endpoint>();
    if (!ep) {
        log::error("xhci: failed to allocate endpoint for slot %u", device->slot_id());
        return nullptr;
    }

    if (ep->init(device->slot_id(), desc) != 0) {
        heap::ufree_delete(ep);
        return nullptr;
    }

    device->set_endpoint(ep->dci(), ep);

    log::info("xhci:   EP%u %s (DCI %u), maxPacket=%u",
               ep->endpoint_num(), ep->is_in() ? "IN" : "OUT",
               ep->dci(), ep->max_packet_size());

    return ep;
}

void xhci_hcd::_configure_endpoint_context(xhci_device* device, xhci_endpoint* ep) {
    auto* input_ctrl = device->input_ctrl_ctx();
    auto* slot_ctx = device->input_slot_ctx();

    input_ctrl->add_flags |= (1u << ep->dci());

    if (ep->dci() > slot_ctx->context_entries) {
        slot_ctx->context_entries = ep->dci();
    }

    auto* ep_ctx = device->input_ep_ctx(ep->dci());
    size_t ep_ctx_size = m_hc_params.csz ? sizeof(xhci_endpoint_context64) : sizeof(xhci_endpoint_context32);
    string::memset(ep_ctx, 0, ep_ctx_size);

    uint8_t xhci_interval = ep->interval();
    uint8_t speed = device->speed();
    uint8_t xfer_type = ep->transfer_type(); // 1=isoch, 3=interrupt
    if (speed == XHCI_USB_SPEED_HIGH_SPEED ||
        speed == XHCI_USB_SPEED_SUPER_SPEED ||
        speed == XHCI_USB_SPEED_SUPER_SPEED_PLUS) {
        if (xhci_interval > 0) {
            xhci_interval--;
        }
    } else if ((speed == XHCI_USB_SPEED_FULL_SPEED || speed == XHCI_USB_SPEED_LOW_SPEED) &&
               (xfer_type == 3 || xfer_type == 1)) {
        // FS/LS interrupt and isochronous: bInterval is in frames (1ms).
        // xHCI Interval is an exponent: period = 2^Interval * 125μs.
        // Convert: microframes = bInterval * 8, Interval = floor(log2(microframes)).
        uint32_t microframes = static_cast<uint32_t>(xhci_interval > 0 ? xhci_interval : 1) * 8;
        uint8_t exponent = 0;
        for (uint32_t v = microframes; v > 1; v >>= 1) exponent++;
        if (exponent < 3) exponent = 3;
        if (exponent > 10) exponent = 10;
        xhci_interval = exponent;
    }

    ep_ctx->endpoint_state = XHCI_ENDPOINT_STATE_DISABLED;
    ep_ctx->endpoint_type = ep->xhc_ep_type();
    ep_ctx->max_packet_size = ep->max_packet_size();
    ep_ctx->max_burst_size = 0;
    ep_ctx->error_count = 3;
    ep_ctx->interval = xhci_interval;
    ep_ctx->average_trb_length = ep->max_packet_size();
    ep_ctx->max_esit_payload_lo = ep->max_packet_size();
    ep_ctx->max_esit_payload_hi = 0;
    ep_ctx->transfer_ring_dequeue_ptr = ep->ring()->get_physical_base();
    ep_ctx->dcs = ep->ring()->get_cycle_bit();
}

int32_t xhci_hcd::_configure_endpoints(xhci_device* device) {
    auto* input_ctrl = device->input_ctrl_ctx();
    input_ctrl->add_flags |= (1u << 0);
    input_ctrl->drop_flags = 0;

    xhci_configure_endpoint_command_trb_t trb = {};
    trb.trb_type = XHCI_TRB_TYPE_CONFIGURE_ENDPOINT_CMD;
    trb.input_context_physical_base = device->input_ctx_phys();
    trb.slot_id = device->slot_id();

    return _send_command(reinterpret_cast<xhci_trb_t*>(&trb));
}

int32_t xhci_hcd::_set_configuration(xhci_device* device, uint8_t config_value) {
    xhci_device_request_packet req = {};
    req.bRequestType = 0x00; // Host to Device, Standard, Device
    req.bRequest = 9;        // SET_CONFIGURATION
    req.wValue = config_value;
    req.wIndex = 0;
    req.wLength = 0;

    return _send_control_transfer(device, req, nullptr, 0);
}

void xhci_hcd::_configure_ctrl_ep_input_context(xhci_device* device, uint16_t max_packet_size) {
    size_t ctx_size = m_hc_params.csz
        ? sizeof(xhci::xhci_input_context64)
        : sizeof(xhci::xhci_input_context32);
    string::memset(device->input_ctrl_ctx(), 0, ctx_size);

    auto* input_ctrl = device->input_ctrl_ctx();
    auto* slot_ctx = device->input_slot_ctx();
    auto* ep0_ctx = device->input_ctrl_ep_ctx();

    input_ctrl->add_flags = (1 << 0) | (1 << 1);
    input_ctrl->drop_flags = 0;

    slot_ctx->route_string = device->route_string();
    slot_ctx->speed = device->speed();
    slot_ctx->context_entries = 1;
    slot_ctx->interrupter_target = 0;

    if (device->route_string() == 0) {
        // Root hub device: port_id is the root hub port number
        slot_ctx->root_hub_port_num = device->port_id();
    } else {
        // Hub-downstream device: use the root port of the topology chain
        slot_ctx->root_hub_port_num = device->root_port_id();

        // xHCI spec Section 6.2.2: parent_hub_slot_id and parent_port_number
        // shall reference the nearest HS hub providing the TT, only for LS/FS
        // devices. Walk up the hub chain to find it.
        if (device->speed() == XHCI_USB_SPEED_LOW_SPEED ||
            device->speed() == XHCI_USB_SPEED_FULL_SPEED) {
            auto* hub = m_slot_devices[device->parent_slot_id()];
            uint8_t port_on_hub = device->parent_port_num();
            while (hub && hub->speed() != XHCI_USB_SPEED_HIGH_SPEED
                       && hub->parent_slot_id() != 0) {
                port_on_hub = hub->parent_port_num();
                hub = m_slot_devices[hub->parent_slot_id()];
            }
            if (hub && hub->speed() == XHCI_USB_SPEED_HIGH_SPEED) {
                slot_ctx->parent_hub_slot_id = hub->slot_id();
                slot_ctx->parent_port_number = port_on_hub;
                slot_ctx->mtt = hub->mtt() ? 1 : 0;
            }
        }
    }

    ep0_ctx->endpoint_state = XHCI_ENDPOINT_STATE_DISABLED;
    ep0_ctx->endpoint_type = XHCI_ENDPOINT_TYPE_CONTROL;
    ep0_ctx->max_packet_size = max_packet_size;
    ep0_ctx->max_burst_size = 0;
    ep0_ctx->error_count = 3;
    ep0_ctx->interval = 0;
    ep0_ctx->average_trb_length = 8;
    ep0_ctx->max_esit_payload_lo = 0;
    ep0_ctx->transfer_ring_dequeue_ptr =
        device->ctrl_ring()->get_physical_base();
    ep0_ctx->dcs = device->ctrl_ring()->get_cycle_bit();
}

int32_t xhci_hcd::_address_device(xhci_device* device, bool bsr) {
    // Construct the Address Device TRB
    xhci_address_device_command_trb_t address_trb;
    address_trb.input_context_physical_base = device->input_ctx_phys();
    address_trb.rsvd = 0;
    address_trb.cycle_bit = 0;
    address_trb.rsvd1 = 0;

    /*
        Block Set Address Request (BSR). When this flag is set to '0' the Address Device Command shall
        generate a USB SET_ADDRESS request to the device. When this flag is set to '1' the Address
        Device Command shall not generate a USB SET_ADDRESS request. Refer to section 4.6.5 for
        more information on the use of this flag.
    */
    address_trb.bsr = bsr ? 1 : 0;

    address_trb.trb_type = XHCI_TRB_TYPE_ADDRESS_DEVICE_CMD;
    address_trb.rsvd2 = 0;
    address_trb.slot_id = device->slot_id();

    return _send_command(reinterpret_cast<xhci_trb_t*>(&address_trb));
}

uint16_t xhci_hcd::_initial_max_packet_size(uint8_t speed) {
    switch (speed) {
    case XHCI_USB_SPEED_LOW_SPEED:
        return 8;
    case XHCI_USB_SPEED_FULL_SPEED:
    case XHCI_USB_SPEED_HIGH_SPEED:
        return 64;
    case XHCI_USB_SPEED_SUPER_SPEED:
    case XHCI_USB_SPEED_SUPER_SPEED_PLUS:
        return 512;
    default:
        return 8;
    }
}

int32_t xhci_hcd::_send_control_transfer(
    xhci_device* device,
    xhci_device_request_packet& request,
    void* buffer, uint32_t length
) {
    xhci_transfer_ring* ring = device->ctrl_ring();

    // Use the device's persistent DMA buffer
    void* dma_buffer = device->ctrl_transfer_buffer();
    uintptr_t dma_buffer_phys = device->ctrl_transfer_buffer_phys();
    if (!dma_buffer || dma_buffer_phys == 0) {
        log::error("xhci: missing control transfer buffer for slot %u", device->slot_id());
        return -1;
    }
    if (length > paging::PAGE_SIZE_4KB) {
        log::error("xhci: control transfer too large (%u bytes)", length);
        return -1;
    }

    bool is_in = (request.transfer_direction != 0);

    // For OUT data stage, copy caller data into DMA buffer before enqueue
    if (length > 0 && !is_in && buffer) {
        string::memcpy(dma_buffer, buffer, length);
    } else {
        string::memset(dma_buffer, 0, length > 0 ? length : 1);
    }

    // Setup Stage TRB
    xhci_setup_stage_trb_t setup = {};
    setup.trb_type = XHCI_TRB_TYPE_SETUP_STAGE;
    setup.request_packet = request;
    setup.trb_transfer_length = 8;
    setup.interrupter_target = 0;
    setup.idt = 1;
    setup.ioc = 0;
    // TRT: 0=No Data, 2=OUT Data, 3=IN Data
    setup.trt = (length > 0) ? (is_in ? 3 : 2) : 0;

    // Data Stage TRB (if there's data to transfer)
    xhci_data_stage_trb_t data = {};
    if (length > 0) {
        data.trb_type = XHCI_TRB_TYPE_DATA_STAGE;
        data.data_buffer = dma_buffer_phys;
        data.trb_transfer_length = length;
        data.td_size = 0;
        data.interrupter_target = 0;
        data.dir = is_in ? 1 : 0;
        data.ioc = 0;
        data.idt = 0;
        data.chain = 0;
    }

    // Status Stage TRB (direction opposite to data stage)
    xhci_status_stage_trb_t status = {};
    status.trb_type = XHCI_TRB_TYPE_STATUS_STAGE;
    status.interrupter_target = 0;
    status.ioc = 1; // Interrupt on completion
    status.dir = (length > 0) ? (is_in ? 0 : 1) : 1;

    // Reset EP0 completion state before doorbell
    device->set_ctrl_completed(false);

    // Enqueue all TRBs before ringing the doorbell
    // (required for QEMU compatibility, also safe on real hardware)
    ring->enqueue(reinterpret_cast<xhci_trb_t*>(&setup));
    if (length > 0) {
        ring->enqueue(reinterpret_cast<xhci_trb_t*>(&data));
    }
    ring->enqueue(reinterpret_cast<xhci_trb_t*>(&status));

    // Ring the control endpoint doorbell
    _ring_doorbell(device->slot_id(), XHCI_DOORBELL_TARGET_CONTROL_EP_RING);

    // Wait for transfer completion
    constexpr uint64_t XFER_TIMEOUT_MS = 5000;
    uint64_t deadline = clock::now_ns() + XFER_TIMEOUT_MS * 1000000ULL;

    _process_event_ring();
    m_event_ring->finish_processing();

    while (!device->ctrl_completed() && clock::now_ns() < deadline) {
        wait_for_event();
        _process_event_ring();
        m_event_ring->finish_processing();
    }

    if (!device->ctrl_completed()) {
        log::error("xhci: control transfer timed out");
        return -1;
    }

    if (device->ctrl_result().completion_code != XHCI_TRB_COMPLETION_CODE_SUCCESS) {
        log::warn("xhci: control transfer failed: %s",
                   trb_completion_code_to_string(device->ctrl_result().completion_code));
        return -1;
    }

    // Copy IN data to caller's buffer
    if (buffer && length > 0 && is_in) {
        barrier::dma_read();
        string::memcpy(buffer, dma_buffer, length);
    }

    return 0;
}

int32_t xhci_hcd::_get_device_descriptor(xhci_device* device, void* out, uint16_t length) {
    xhci_device_request_packet req = {};
    req.bRequestType = 0x80; // Device to Host, Standard, Device
    req.bRequest = 6;        // GET_DESCRIPTOR
    req.wValue = usb::USB_DESCRIPTOR_REQUEST(usb::USB_DESCRIPTOR_DEVICE, 0);
    req.wIndex = 0;
    req.wLength = length;

    return _send_control_transfer(device, req, out, length);
}

int32_t xhci_hcd::_get_configuration_descriptor(
    xhci_device* device,
    usb::usb_configuration_descriptor* out,
    uint8_t config_index
) {
    xhci_device_request_packet req = {};
    req.bRequestType = 0x80;
    req.bRequest = 6; // GET_DESCRIPTOR
    req.wValue = usb::USB_DESCRIPTOR_REQUEST(usb::USB_DESCRIPTOR_CONFIGURATION, config_index);
    req.wIndex = 0;

    // First pass: read the 9-byte config descriptor header to get wTotalLength
    constexpr uint16_t CONFIG_HDR_SIZE = 9; // bLength + bDescriptorType + wTotalLength + 5 fields
    req.wLength = CONFIG_HDR_SIZE;
    if (_send_control_transfer(device, req, out, CONFIG_HDR_SIZE) != 0) {
        return -1;
    }

    // Second pass: read the full descriptor
    uint16_t total_length = out->wTotalLength;
    if (total_length > sizeof(usb::usb_configuration_descriptor)) {
        log::warn("xhci: config descriptor too large (%u bytes), clamping", total_length);
        total_length = sizeof(usb::usb_configuration_descriptor);
    }

    req.wLength = total_length;
    return _send_control_transfer(device, req, out, total_length);
}

int32_t xhci_hcd::_submit_normal_transfer(
    xhci_device* device, xhci_endpoint* ep,
    void* buffer, uint32_t length
) {
    if (!ep || !ep->ring() || !ep->dma_buffer()) {
        return -1;
    }
    if (length > paging::PAGE_SIZE_4KB) {
        log::error("xhci: normal transfer too large (%u bytes)", length);
        return -1;
    }

    // Reset completion state before doorbell to avoid race with HCD event dispatch
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(ep->completion_lock());
        ep->set_completed(false);
        sync::spin_unlock_irqrestore(ep->completion_lock(), irq);
    });

    // Copy OUT data into DMA buffer before enqueue
    if (!ep->is_in() && buffer && length > 0) {
        string::memcpy(ep->dma_buffer(), buffer, length);
    }

    if (!ep->ring()->can_enqueue(1)) {
        log::error("xhci: transfer ring full for EP%u", ep->endpoint_num());
        return -1;
    }

    xhci_normal_trb_t normal = {};
    normal.trb_type = XHCI_TRB_TYPE_NORMAL;
    normal.data_buffer_physical_base = ep->dma_buffer_phys();
    normal.trb_transfer_length = length;
    normal.td_size = 0;
    normal.interrupter_target = 0;
    normal.ioc = 1;
    normal.isp = ep->is_in() ? 1 : 0;
    normal.chain = 0;

    ep->ring()->enqueue(reinterpret_cast<xhci_trb_t*>(&normal));
    _ring_doorbell(device->slot_id(), ep->dci());

    // Wait for transfer completion (HCD task processes events and wakes us)
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(ep->completion_lock());
        while (!ep->completed()) {
            irq = sync::wait(ep->completion_wq(), ep->completion_lock(), irq);
        }
        sync::spin_unlock_irqrestore(ep->completion_lock(), irq);
    });

    // Copy IN data from DMA buffer to caller
    if (ep->is_in() && buffer && length > 0) {
        barrier::dma_read();
        string::memcpy(buffer, ep->dma_buffer(), length);
    }

    if (ep->result().completion_code != XHCI_TRB_COMPLETION_CODE_SUCCESS &&
        ep->result().completion_code != XHCI_TRB_COMPLETION_CODE_SHORT_PACKET) {
        log::warn("xhci: normal transfer failed on EP%u: %s",
                   ep->endpoint_num(),
                   trb_completion_code_to_string(ep->result().completion_code));
        return -1;
    }

    return 0;
}

int32_t xhci_hcd::usb_control_transfer(
    xhci_device* device,
    uint8_t request_type, uint8_t request,
    uint16_t value, uint16_t index,
    void* data, uint16_t length
) {
    // If called from the HCD task, use the internal event-processing path.
    // If called from a class driver task, use the wait-queue path so we
    // don't compete with the HCD for event ring processing.
    if (sched::current() == m_task) {
        xhci_device_request_packet req = {};
        req.bRequestType = request_type;
        req.bRequest = request;
        req.wValue = value;
        req.wIndex = index;
        req.wLength = length;
        return _send_control_transfer(device, req, data, length);
    }

    xhci_transfer_ring* ring = device->ctrl_ring();

    void* dma_buffer = device->ctrl_transfer_buffer();
    uintptr_t dma_buffer_phys = device->ctrl_transfer_buffer_phys();
    if (!dma_buffer || dma_buffer_phys == 0) {
        log::error("xhci: missing control transfer buffer for slot %u", device->slot_id());
        return -1;
    }
    if (length > paging::PAGE_SIZE_4KB) {
        log::error("xhci: control transfer too large (%u bytes)", length);
        return -1;
    }

    xhci_device_request_packet req = {};
    req.bRequestType = request_type;
    req.bRequest = request;
    req.wValue = value;
    req.wIndex = index;
    req.wLength = length;

    bool is_in = (req.transfer_direction != 0);

    if (length > 0 && !is_in && data) {
        string::memcpy(dma_buffer, data, length);
    } else {
        string::memset(dma_buffer, 0, length > 0 ? length : 1);
    }

    xhci_setup_stage_trb_t setup = {};
    setup.trb_type = XHCI_TRB_TYPE_SETUP_STAGE;
    setup.request_packet = req;
    setup.trb_transfer_length = 8;
    setup.interrupter_target = 0;
    setup.idt = 1;
    setup.ioc = 0;
    setup.trt = (length > 0) ? (is_in ? 3 : 2) : 0;

    xhci_data_stage_trb_t data_trb = {};
    if (length > 0) {
        data_trb.trb_type = XHCI_TRB_TYPE_DATA_STAGE;
        data_trb.data_buffer = dma_buffer_phys;
        data_trb.trb_transfer_length = length;
        data_trb.td_size = 0;
        data_trb.interrupter_target = 0;
        data_trb.dir = is_in ? 1 : 0;
        data_trb.ioc = 0;
        data_trb.idt = 0;
        data_trb.chain = 0;
    }

    xhci_status_stage_trb_t status = {};
    status.trb_type = XHCI_TRB_TYPE_STATUS_STAGE;
    status.interrupter_target = 0;
    status.ioc = 1;
    status.dir = (length > 0) ? (is_in ? 0 : 1) : 1;

    // Reset EP0 completion under the lock before ringing the doorbell
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(device->ctrl_completion_lock());
        device->set_ctrl_completed(false);
        sync::spin_unlock_irqrestore(device->ctrl_completion_lock(), irq);
    });

    ring->enqueue(reinterpret_cast<xhci_trb_t*>(&setup));
    if (length > 0) {
        ring->enqueue(reinterpret_cast<xhci_trb_t*>(&data_trb));
    }
    ring->enqueue(reinterpret_cast<xhci_trb_t*>(&status));

    _ring_doorbell(device->slot_id(), XHCI_DOORBELL_TARGET_CONTROL_EP_RING);

    // Block on the per-device EP0 completion wait queue.
    // The HCD event loop processes the transfer completion and wakes us.
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(device->ctrl_completion_lock());
        while (!device->ctrl_completed()) {
            irq = sync::wait(device->ctrl_completion_wq(),
                             device->ctrl_completion_lock(), irq);
        }
        sync::spin_unlock_irqrestore(device->ctrl_completion_lock(), irq);
    });

    if (device->ctrl_result().completion_code != XHCI_TRB_COMPLETION_CODE_SUCCESS) {
        log::warn("xhci: control transfer failed: %s",
                   trb_completion_code_to_string(device->ctrl_result().completion_code));
        return -1;
    }

    if (data && length > 0 && is_in) {
        barrier::dma_read();
        string::memcpy(data, dma_buffer, length);
    }

    return 0;
}

int32_t xhci_hcd::usb_submit_transfer(
    xhci_device* device, uint8_t endpoint_addr,
    void* buffer, uint32_t length
) {
    auto* ep = device->endpoint_by_address(endpoint_addr);
    if (!ep) {
        log::error("xhci: no endpoint for address 0x%02x on slot %u",
                   endpoint_addr, device->slot_id());
        return -1;
    }
    return _submit_normal_transfer(device, ep, buffer, length);
}

REGISTER_PCI_DRIVER(xhci_hcd,
    PCI_MATCH(PCI_MATCH_ANY, PCI_MATCH_ANY, 0x0C, 0x03, 0x30),
    PCI_DRIVER_FACTORY(xhci_hcd));

} // namespace drivers
