#include "xhci.h"
#include <paging/page.h>
#include <paging/phys_addr_translation.h>
#include <paging/tlb.h>
#include <memory/kmemory.h>
#include <time/ktime.h>
#include <arch/x86/ioapic.h>
#include <interrupts/interrupts.h>
#include <kprint.h>

namespace drivers {
    XhciDriver g_globalXhciInstance;

    void* _allocXhciMemory(size_t size, size_t alignment = 64, size_t boundary = PAGE_SIZE) {
        // Allocate extra memory to ensure we can align the block within the boundary
        size_t totalSize = size + boundary - 1;
        void* memblock = kmallocAligned(totalSize, alignment);

        if (!memblock) {
            kuPrint("[XHCI] ======= MEMORY ALLOCATION PROBLEM =======\n");
            while (true);
        }

        // Align the memory block to the specified boundary
        size_t alignedAddress = ((size_t)memblock + boundary - 1) & ~(boundary - 1);
        void* aligned = (void*)alignedAddress;

        // Mark the aligned memory block as uncacheable
        paging::markPageUncacheable(aligned);

        return aligned;
    }

    const char* extendedCapabilityToString(XhciExtendedCapabilityCode capid) {
        uint8_t id = static_cast<uint8_t>(capid);

        switch (capid) {
        case XhciExtendedCapabilityCode::Reserved: return "Reserved";
        case XhciExtendedCapabilityCode::UsbLegacySupport: return "USB Legacy Support";
        case XhciExtendedCapabilityCode::SupportedProtocol: return "Supported Protocol";
        case XhciExtendedCapabilityCode::ExtendedPowerManagement: return "Extended Power Management";
        case XhciExtendedCapabilityCode::IOVirtualizationSupport: return "I/O Virtualization Support";
        case XhciExtendedCapabilityCode::LocalMemorySupport: return "Local Memory Support";
        case XhciExtendedCapabilityCode::UsbDebugCapabilitySupport: return "USB Debug Capability Support";
        case XhciExtendedCapabilityCode::ExtendedMessageInterruptSupport: return "Extended Message Interrupt Support";
        default: break;
        }

        if (id >= 7 && id <= 9) {
            return "Reserved";
        }

        if (id >= 11 && id <= 16) {
            return "Reserved";
        }

        if (id >= 18 && id <= 191) {
            return "Reserved";
        }

        return "Vendor Specific";
    }

    const char* trbCompletionCodeToString(uint8_t completionCode) {
        switch (completionCode) {
        case XHCI_TRB_COMPLETION_CODE_INVALID:
            return "INVALID";
        case XHCI_TRB_COMPLETION_CODE_SUCCESS:
            return "SUCCESS";
        case XHCI_TRB_COMPLETION_CODE_DATA_BUFFER_ERROR:
            return "DATA_BUFFER_ERROR";
        case XHCI_TRB_COMPLETION_CODE_BABBLE_DETECTED_ERROR:
            return "BABBLE_DETECTED_ERROR";
        case XHCI_TRB_COMPLETION_CODE_USB_TRANSACTION_ERROR:
            return "USB_TRANSACTION_ERROR";
        case XHCI_TRB_COMPLETION_CODE_TRB_ERROR:
            return "TRB_ERROR";
        case XHCI_TRB_COMPLETION_CODE_STALL_ERROR:
            return "STALL_ERROR";
        case XHCI_TRB_COMPLETION_CODE_RESOURCE_ERROR:
            return "RESOURCE_ERROR";
        case XHCI_TRB_COMPLETION_CODE_BANDWIDTH_ERROR:
            return "BANDWIDTH_ERROR";
        case XHCI_TRB_COMPLETION_CODE_NO_SLOTS_AVAILABLE:
            return "NO_SLOTS_AVAILABLE";
        case XHCI_TRB_COMPLETION_CODE_INVALID_STREAM_TYPE:
            return "INVALID_STREAM_TYPE";
        case XHCI_TRB_COMPLETION_CODE_SLOT_NOT_ENABLED:
            return "SLOT_NOT_ENABLED";
        case XHCI_TRB_COMPLETION_CODE_ENDPOINT_NOT_ENABLED:
            return "ENDPOINT_NOT_ENABLED";
        case XHCI_TRB_COMPLETION_CODE_SHORT_PACKET:
            return "SHORT_PACKET";
        case XHCI_TRB_COMPLETION_CODE_RING_UNDERRUN:
            return "RING_UNDERRUN";
        case XHCI_TRB_COMPLETION_CODE_RING_OVERRUN:
            return "RING_OVERRUN";
        case XHCI_TRB_COMPLETION_CODE_VF_EVENT_RING_FULL:
            return "VF_EVENT_RING_FULL";
        case XHCI_TRB_COMPLETION_CODE_PARAMETER_ERROR:
            return "PARAMETER_ERROR";
        case XHCI_TRB_COMPLETION_CODE_BANDWIDTH_OVERRUN:
            return "BANDWIDTH_OVERRUN";
        case XHCI_TRB_COMPLETION_CODE_CONTEXT_STATE_ERROR:
            return "CONTEXT_STATE_ERROR";
        case XHCI_TRB_COMPLETION_CODE_NO_PING_RESPONSE:
            return "NO_PING_RESPONSE";
        case XHCI_TRB_COMPLETION_CODE_EVENT_RING_FULL:
            return "EVENT_RING_FULL";
        case XHCI_TRB_COMPLETION_CODE_INCOMPATIBLE_DEVICE:
            return "INCOMPATIBLE_DEVICE";
        case XHCI_TRB_COMPLETION_CODE_MISSED_SERVICE:
            return "MISSED_SERVICE";
        case XHCI_TRB_COMPLETION_CODE_COMMAND_RING_STOPPED:
            return "COMMAND_RING_STOPPED";
        case XHCI_TRB_COMPLETION_CODE_COMMAND_ABORTED:
            return "COMMAND_ABORTED";
        case XHCI_TRB_COMPLETION_CODE_STOPPED:
            return "STOPPED";
        case XHCI_TRB_COMPLETION_CODE_STOPPED_LENGTH_INVALID:
            return "STOPPED_LENGTH_INVALID";
        case XHCI_TRB_COMPLETION_CODE_STOPPED_SHORT_PACKET:
            return "STOPPED_SHORT_PACKET";
        case XHCI_TRB_COMPLETION_CODE_MAX_EXIT_LATENCY_ERROR:
            return "MAX_EXIT_LATENCY_ERROR";
        default:
            return "UNKNOWN_COMPLETION_CODE";
        }
    }

    XhciExtendedCapability::XhciExtendedCapability(volatile uint32_t* capPtr)
    : m_base(capPtr) {
        m_entry.raw = *m_base;
        _readNextExtCaps();
    }

    void XhciExtendedCapability::_readNextExtCaps() {
        if (m_entry.next) {
            auto nextCapPtr = XHCI_NEXT_EXT_CAP_PTR(m_base, m_entry.next);
            m_next = kstl::SharedPtr<XhciExtendedCapability>(
                new XhciExtendedCapability(nextCapPtr)
            );
        }
    }

    void XhciPortRegisterManager::readPortscReg(XhciPortscRegister& reg) const {
        uint64_t portscAddress = m_base + m_portscOffset;
        reg.raw = *reinterpret_cast<volatile uint32_t*>(portscAddress);
    }

    void XhciPortRegisterManager::writePortscReg(XhciPortscRegister& reg) const {
        uint64_t portscAddress = m_base + m_portscOffset;
        *reinterpret_cast<volatile uint32_t*>(portscAddress) = reg.raw;
    }

    void XhciPortRegisterManager::readPortpmscRegUsb2(XhciPortpmscRegisterUsb2& reg) const {
        uint64_t portpmscAddress = m_base + m_portpmscOffset;
        reg.raw = *reinterpret_cast<volatile uint32_t*>(portpmscAddress);
    }

    void XhciPortRegisterManager::writePortpmscRegUsb2(XhciPortpmscRegisterUsb2& reg) const {
        uint64_t portpmscAddress = m_base + m_portpmscOffset;
        *reinterpret_cast<volatile uint32_t*>(portpmscAddress) = reg.raw;
    }

    void XhciPortRegisterManager::readPortpmscRegUsb3(XhciPortpmscRegisterUsb3& reg) const {
        uint64_t portpmscAddress = m_base + m_portpmscOffset;
        reg.raw = *reinterpret_cast<volatile uint32_t*>(portpmscAddress);
    }

    void XhciPortRegisterManager::writePortpmscRegUsb3(XhciPortpmscRegisterUsb3& reg) const {
        uint64_t portpmscAddress = m_base + m_portpmscOffset;
        *reinterpret_cast<volatile uint32_t*>(portpmscAddress) = reg.raw;
    }

    void XhciPortRegisterManager::readPortliReg(XhciPortliRegister& reg) const {
        uint64_t portliAddress = m_base + m_portliOffset;
        reg.raw = *reinterpret_cast<volatile uint32_t*>(portliAddress);
    }

    void XhciPortRegisterManager::writePortliReg(XhciPortliRegister& reg) const {
        uint64_t portliAddress = m_base + m_portliOffset;
        *reinterpret_cast<volatile uint32_t*>(portliAddress) = reg.raw;
    }

    void XhciPortRegisterManager::readPorthlpmcRegUsb2(XhciPorthlpmcRegisterUsb2& reg) const {
        uint64_t porthlpmAddress = m_base + m_porthlpmcOffset;
        reg.raw = *reinterpret_cast<volatile uint32_t*>(porthlpmAddress);
    }

    void XhciPortRegisterManager::writePorthlpmcRegUsb2(XhciPorthlpmcRegisterUsb2& reg) const {
        uint64_t porthlpmAddress = m_base + m_porthlpmcOffset;
        *reinterpret_cast<volatile uint32_t*>(porthlpmAddress) = reg.raw;
    }

    void XhciPortRegisterManager::readPorthlpmcRegUsb3(XhciPorthlpmcRegisterUsb3& reg) const {
        uint64_t porthlpmAddress = m_base + m_porthlpmcOffset;
        reg.raw = *reinterpret_cast<volatile uint32_t*>(porthlpmAddress);
    }

    void XhciPortRegisterManager::writePorthlpmcRegUsb3(XhciPorthlpmcRegisterUsb3& reg) const {
        uint64_t porthlpmAddress = m_base + m_porthlpmcOffset;
        *reinterpret_cast<volatile uint32_t*>(porthlpmAddress) = reg.raw;
    }

    XhciInterrupterRegisters* XhciRuntimeRegisterManager::getInterrupterRegisters(uint8_t interrupter) const {
        if (interrupter > m_maxInterrupters) {
            return nullptr;
        }

        return &m_base->ir[interrupter];
    }

    XhciDoorbellManager::XhciDoorbellManager(uint64_t base) {
        m_doorbellRegisters = reinterpret_cast<XhciDoorbellRegister*>(base);
    }

    void XhciDoorbellManager::ringDoorbell(uint8_t target) {
        // *Note* Write _0_ to send the command
        m_doorbellRegisters[target].raw = 0;
    }

    void XhciDoorbellManager::ringCommandDoorbell() {
        uint8_t target = XHCI_DOORBELL_TARGET_COMMAND_RING;
        ringDoorbell(target);
    }

    XhciCommandRing::XhciCommandRing(size_t maxTrbs) {
        m_maxTrbCount = maxTrbs;
        m_rcsBit = XHCI_CRCR_RING_CYCLE_STATE;
        m_enqueuePtr = 0;

        const uint64_t ringSize = maxTrbs * sizeof(XhciTrb_t);

        // Create the command ring memory block
        m_trbRing = (XhciTrb_t*)_allocXhciMemory(
            ringSize,
            XHCI_COMMAND_RING_SEGMENTS_ALIGNMENT,
            XHCI_COMMAND_RING_SEGMENTS_BOUNDARY
        );

        // Zero out the memory by default
        zeromem(m_trbRing, ringSize);

        // Get the physical mapping
        m_physicalRingBase = (uint64_t)__pa(m_trbRing);

        // Set the last TRB as a link TRB to point back to the first TRB
        m_trbRing[255].parameter = m_physicalRingBase;
        m_trbRing[255].control = (XHCI_TRB_TYPE_LINK << 10) | m_rcsBit;
    }

    void XhciCommandRing::enqueue(XhciTrb_t& trb) {
        // Adjust the TRB's cycle bit to the current RCS
        trb.cycleBit = m_rcsBit;

        // Insert the TRB into the ring
        m_trbRing[m_enqueuePtr] = trb;

        // Advance and possibly wrap the enqueue pointer if needed.
        // maxTrbCount - 1 accounts for the LINK_TRB.
        if (++m_enqueuePtr == m_maxTrbCount - 1) {
            m_enqueuePtr = 0;
            m_rcsBit = !m_rcsBit;
        }
    }

    void XhciCommandRing::enqueueEnableSlotTrb() {
        XhciTrb_t trb = XHCI_CONSTRUCT_CMD_TRB(XHCI_TRB_TYPE_ENABLE_SLOT_CMD);
        enqueue(trb);
    }

    XhciEventRing::XhciEventRing(size_t maxTrbs, XhciInterrupterRegisters* primaryInterrupterRegisters) {
        m_interrupterRegs = primaryInterrupterRegisters;
        m_segmentTrbCount = maxTrbs;
        m_rcsBit = XHCI_CRCR_RING_CYCLE_STATE;
        m_dequeuePtr = 0;

        const uint64_t eventRingSegmentSize = maxTrbs * sizeof(XhciTrb_t);
        const uint64_t eventRingSegmentTableSize = m_segmentCount * sizeof(XhciErstEntry);

        // Create the event ring segment memory block
        m_primarySegmentRing = (XhciTrb_t*)_allocXhciMemory(
            eventRingSegmentSize,
            XHCI_EVENT_RING_SEGMENTS_ALIGNMENT,
            XHCI_EVENT_RING_SEGMENTS_BOUNDARY
        );

        // Zero out the memory by default
        zeromem(m_primarySegmentRing, eventRingSegmentSize);

        // Get the physical mapping to the main event ring segment
        m_primarySegmentPhysicalBase = (uint64_t)__pa(m_primarySegmentRing);

        // Create the event ring segment table
        m_segmentTable = (XhciErstEntry*)_allocXhciMemory(
            eventRingSegmentTableSize,
            XHCI_EVENT_RING_SEGMENT_TABLE_ALIGNMENT,
            XHCI_EVENT_RING_SEGMENT_TABLE_BOUNDARY
        );

        // Get the physical mapping to the segment table
        m_segmentTablePhysicalBase = (uint64_t)__pa(m_segmentTable);

        // Construct the segment table entry
        XhciErstEntry entry;
        entry.ringSegmentBaseAddress = m_primarySegmentPhysicalBase;
        entry.ringSegmentSize = XHCI_EVENT_RING_TRB_COUNT;
        entry.rsvd = 0;

        // Insert the constructed segment into the table
        m_segmentTable[0] = entry;

        // Configure the Event Ring Segment Table Size (ERSTSZ) register
        m_interrupterRegs->erstsz = 1;

        // Initialize and set ERDP
        _updateErdpInterrupterRegister();

        // Write to ERSTBA register
        m_interrupterRegs->erstba = m_segmentTablePhysicalBase;
    }

    bool XhciEventRing::hasUnprocessedEvents() {
        return (m_primarySegmentRing[m_dequeuePtr].cycleBit == m_rcsBit);
    }

    void XhciEventRing::dequeueEvents(kstl::vector<XhciTrb_t*>& receivedEventTrbs) {
        // Process each event TRB
        while (hasUnprocessedEvents()) {
            auto trb = _dequeueTrb();
            if (!trb) break;

            receivedEventTrbs.pushBack(trb);
        }

        // Update the ERDP register
        _updateErdpInterrupterRegister();
    }

    void XhciEventRing::flushUnprocessedEvents() {
        // Dequeue all unprocessed TRBs
        while (hasUnprocessedEvents()) {
            _dequeueTrb();
        }

        // Update the ERDP register
        _updateErdpInterrupterRegister();
    }

    void XhciEventRing::_updateErdpInterrupterRegister() {
        uint64_t dequeueAddress = m_primarySegmentPhysicalBase + (m_dequeuePtr * sizeof(XhciTrb_t));
        m_interrupterRegs->erdp = dequeueAddress;
    }

    XhciTrb_t* XhciEventRing::_dequeueTrb() {
        if (m_primarySegmentRing[m_dequeuePtr].cycleBit != m_rcsBit) {
            kprintWarn("[XHCI_EVENT_RING] Dequeued an invalid TRB, returning NULL!\n");
            return nullptr;
        }

        // Get the resulting TRB
        XhciTrb_t* ret = &m_primarySegmentRing[m_dequeuePtr];

        // Advance and possibly wrap the dequeue pointer if needed
        if (++m_dequeuePtr == m_segmentTrbCount) {
            m_dequeuePtr = 0;
            m_rcsBit = !m_rcsBit;
        }

        return ret;
    }

    XhciTransferRing::XhciTransferRing(size_t maxTrbs) {
        m_maxTrbCount = maxTrbs;
        m_dcsBit = 1;
        m_dequeuePtr = 0;
        m_enqueuePtr = 0;

        const uint64_t ringSize = maxTrbs * sizeof(XhciTrb_t);

        // Create the transfer ring memory block
        m_trbRing = (XhciTrb_t*)_allocXhciMemory(
            ringSize,
            XHCI_TRANSFER_RING_SEGMENTS_ALIGNMENT,
            XHCI_TRANSFER_RING_SEGMENTS_BOUNDARY
        );

        // Zero out the memory by default
        zeromem(m_trbRing, ringSize);

        // Get the physical mapping
        m_physicalRingBase = (uint64_t)__pa(m_trbRing);

        // Set the last TRB as a link TRB to point back to the first TRB
        m_trbRing[255].parameter = m_physicalRingBase;
        m_trbRing[255].control = (XHCI_TRB_TYPE_LINK << 10) | m_dcsBit;
    }

    XhciDriver& XhciDriver::get() {
        return g_globalXhciInstance;
    }

    bool XhciDriver::init(PciDeviceInfo& deviceInfo) {
        _mapDeviceMmio(deviceInfo.barAddress);

        // Parse the read-only capability register space
        _parseCapabilityRegisters();
        _logCapabilityRegisters();

        // Parse the extended capabilities
        _parseExtendedCapabilityRegisters();

        // Reset the controller
        if (!_resetHostController()) {
            return false;
        }

        // Configure the controller's register spaces
        _configureOperationalRegisters();
        _configureRuntimeRegisters();

        // At this point the controller is all setup so we can start it
        _startHostController();

        // Reset the ports
        for (uint8_t i = 0; i < m_maxPorts; i++) {
            if (_resetPort(i)) {
                kprintInfo("[*] Successfully reset %s port %i\n", _isUSB3Port(i) ? "USB3" : "USB2", i);
            } else {
                kprintWarn("[*] Failed to reset %s port %i\n", _isUSB3Port(i) ? "USB3" : "USB2", i);
            }
        }
        kprint("\n");

        // After port resets, there will be extreneous port state change events
        // for ports with connected devices, but without CSC bit set, so we have
        // to manually iterate the ports with connected devices and set them up.
        m_eventRing->flushUnprocessedEvents();

        for (uint8_t port = 0; port < m_maxPorts; ++port) {
            auto portRegisterSet = _getPortRegisterSet(port);
            XhciPortscRegister portsc;
            portRegisterSet.readPortscReg(portsc);

            if (portsc.ccs) {
                _handleDeviceConnected(port);
            }
        }

        kstl::vector<XhciTrb_t*> eventTrbs;
        while (true) {
           eventTrbs.clear();
            if (m_eventRing->hasUnprocessedEvents()) {
                m_eventRing->dequeueEvents(eventTrbs);
                _markXhciInterruptCompleted(0);
            }

            // Process the TRBs
            for (size_t i = 0; i < eventTrbs.size(); ++i) {
                _processEventRingTrb(eventTrbs[i]);
            }
        }

        kprint("\n");
        return true;
    }

    void XhciDriver::_parseCapabilityRegisters() {
        m_capRegs = reinterpret_cast<volatile XhciCapabilityRegisters*>(m_xhcBase);

        m_capabilityRegsLength = m_capRegs->caplength;

        m_maxDeviceSlots = XHCI_MAX_DEVICE_SLOTS(m_capRegs);
        m_maxInterrupters = XHCI_MAX_INTERRUPTERS(m_capRegs);
        m_maxPorts = XHCI_MAX_PORTS(m_capRegs);

        m_isochronousSchedulingThreshold = XHCI_IST(m_capRegs);
        m_erstMax = XHCI_ERST_MAX(m_capRegs);
        m_maxScratchpadBuffers = XHCI_MAX_SCRATCHPAD_BUFFERS(m_capRegs);

        m_64bitAddressingCapability = XHCI_AC64(m_capRegs);
        m_bandwidthNegotiationCapability = XHCI_BNC(m_capRegs);
        m_64ByteContextSize = XHCI_CSZ(m_capRegs);
        m_portPowerControl = XHCI_PPC(m_capRegs);
        m_portIndicators = XHCI_PIND(m_capRegs);
        m_lightResetCapability = XHCI_LHRC(m_capRegs);
        m_extendedCapabilitiesOffset = XHCI_XECP(m_capRegs) * sizeof(uint32_t);

        // Update the base pointer to operational register set
        m_opRegs = reinterpret_cast<volatile XhciOperationalRegisters*>(m_xhcBase + m_capabilityRegsLength);

        // Construct a manager class instance for the doorbell register array
        m_doorbellManager = kstl::SharedPtr<XhciDoorbellManager>(
            new XhciDoorbellManager(m_xhcBase + m_capRegs->dboff)
        );

        // Construct a controller class instance for the runtime register set
        uint64_t runtimeRegisterBase = m_xhcBase + m_capRegs->rtsoff;
        m_runtimeRegisterManager = kstl::SharedPtr<XhciRuntimeRegisterManager>(
            new XhciRuntimeRegisterManager(runtimeRegisterBase, m_maxInterrupters)
        );
    }

    void XhciDriver::_logCapabilityRegisters() {
        kprintInfo("===== Capability Registers (0x%llx) =====\n", (uint64_t)m_capRegs);
        kprintInfo("    Length                : %i\n", m_capabilityRegsLength);
        kprintInfo("    Max Device Slots      : %i\n", m_maxDeviceSlots);
        kprintInfo("    Max Interrupters      : %i\n", m_maxInterrupters);
        kprintInfo("    Max Ports             : %i\n", m_maxPorts);
        kprintInfo("    IST                   : %i\n", m_isochronousSchedulingThreshold);
        kprintInfo("    ERST Max Size         : %i\n", m_erstMax);
        kprintInfo("    Scratchpad Buffers    : %i\n", m_maxScratchpadBuffers);
        kprintInfo("    64-bit Addressing     : %i\n", m_64bitAddressingCapability);
        kprintInfo("    Bandwidth Negotiation : %i\n", m_bandwidthNegotiationCapability);
        kprintInfo("    64-byte Context Size  : %i\n", m_64ByteContextSize);
        kprintInfo("    Port Power Control    : %i\n", m_portPowerControl);
        kprintInfo("    Port Indicators       : %i\n", m_portIndicators);
        kprintInfo("    Light Reset Available : %i\n", m_lightResetCapability);
        kprint("\n");
    }

    void XhciDriver::_parseExtendedCapabilityRegisters() {
        volatile uint32_t* headCapPtr = reinterpret_cast<volatile uint32_t*>(
            m_xhcBase + m_extendedCapabilitiesOffset
        );

        m_extendedCapabilitiesHead = kstl::SharedPtr<XhciExtendedCapability>(
            new XhciExtendedCapability(headCapPtr)
        );

        auto node = m_extendedCapabilitiesHead;
        while (node.get()) {
            if (node->id() == XhciExtendedCapabilityCode::SupportedProtocol) {
                XhciUsbSupportedProtocolCapability cap(node->base());
                // Make the ports zero-based
                uint8_t firstPort = cap.compatiblePortOffset - 1;
                uint8_t lastPort = firstPort + cap.compatiblePortCount - 1;

                if (cap.majorRevisionVersion == 3) {
                    for (uint8_t port = firstPort; port <= lastPort; port++) {
                        m_usb3Ports.pushBack(port);
                    }
                }
            }
            node = node->next();
        }
    }

    void XhciDriver::_configureOperationalRegisters() {
        // Establish host controller's supported page size in bytes
        m_hcPageSize = static_cast<uint64_t>(m_opRegs->pagesize & 0xffff) << 12;
        
        // Enable device notifications 
        m_opRegs->dnctrl = 0xffff;

        // Configure the usbconfig field
        m_opRegs->config = static_cast<uint32_t>(m_maxDeviceSlots);

        // Setup device context base address array and scratchpad buffers
        _setupDcbaa();

        // Setup the command ring and write CRCR
        m_commandRing = kstl::SharedPtr<XhciCommandRing>(
            new XhciCommandRing(XHCI_COMMAND_RING_TRB_COUNT)
        );
        m_opRegs->crcr = m_commandRing->getPhysicalBase() | m_commandRing->getCycleBit();
    }

    void XhciDriver::_logUsbsts() {
        uint32_t status = m_opRegs->usbsts;
        kprint("===== USBSTS =====\n");
        if (status & XHCI_USBSTS_HCH) kprint("    Host Controlled Halted\n");
        if (status & XHCI_USBSTS_HSE) kprint("    Host System Error\n");
        if (status & XHCI_USBSTS_EINT) kprint("    Event Interrupt\n");
        if (status & XHCI_USBSTS_PCD) kprint("    Port Change Detect\n");
        if (status & XHCI_USBSTS_SSS) kprint("    Save State Status\n");
        if (status & XHCI_USBSTS_RSS) kprint("    Restore State Status\n");
        if (status & XHCI_USBSTS_SRE) kprint("    Save/Restore Error\n");
        if (status & XHCI_USBSTS_CNR) kprint("    Controller Not Ready\n");
        if (status & XHCI_USBSTS_HCE) kprint("    Host Controller Error\n");
        kprint("\n");
    }

    void XhciDriver::_logOperationalRegisters() {
        kprintInfo("===== Operational Registers (0x%llx) =====\n", (uint64_t)m_opRegs);
        kprintInfo("    usbcmd     : %x\n", m_opRegs->usbcmd);
        kprintInfo("    usbsts     : %x\n", m_opRegs->usbsts);
        kprintInfo("    pagesize   : %x\n", m_opRegs->pagesize);
        kprintInfo("    dnctrl     : %x\n", m_opRegs->dnctrl);
        kprintInfo("    crcr       : %llx\n", m_opRegs->crcr);
        kprintInfo("    dcbaap     : %llx\n", m_opRegs->dcbaap);
        kprintInfo("    config     : %x\n", m_opRegs->config);
        kprint("\n");
    }

    void XhciDriver::_configureRuntimeRegisters() {
        // Get the primary interrupter registers
        auto interrupterRegs = m_runtimeRegisterManager->getInterrupterRegisters(0);
        if (!interrupterRegs) {
            kprintError("[*] Failed to retrieve interrupter register set when setting up the event ring!");
            return;
        }

        // Setup the event ring and write to interrupter
        // registers to set ERSTSZ, ERSDP, and ERSTBA.
        m_eventRing = kstl::SharedPtr<XhciEventRing>(
            new XhciEventRing(XHCI_EVENT_RING_TRB_COUNT, interrupterRegs)
        );

        // Clear any pending interrupts for primary interrupter
        _markXhciInterruptCompleted(0);
    }

    bool XhciDriver::_isUSB3Port(uint8_t portNum) {
        for (size_t i = 0; i < m_usb3Ports.size(); ++i) {
            if (m_usb3Ports[i] == portNum) {
                return true;
            }
        }

        return false;
    }

    XhciPortRegisterManager XhciDriver::_getPortRegisterSet(uint8_t portNum) {
        uint64_t base = reinterpret_cast<uint64_t>(m_opRegs) + (0x400 + (0x10 * portNum));
        return XhciPortRegisterManager(base);
    }

    void XhciDriver::_setupDcbaa() {
        size_t contextEntrySize = m_64ByteContextSize ? 64 : 32;
        size_t dcbaaSize = contextEntrySize * (m_maxDeviceSlots + 1);

        m_dcbaa = (uint64_t*)_allocXhciMemory(dcbaaSize, XHCI_DEVICE_CONTEXT_ALIGNMENT, XHCI_DEVICE_CONTEXT_BOUNDARY);
        zeromem(m_dcbaa, dcbaaSize);

        /*
        // xHci Spec Section 6.1 (page 404)

        If the Max Scratchpad Buffers field of the HCSPARAMS2 register is > ‘0’, then
        the first entry (entry_0) in the DCBAA shall contain a pointer to the Scratchpad
        Buffer Array. If the Max Scratchpad Buffers field of the HCSPARAMS2 register is
        = ‘0’, then the first entry (entry_0) in the DCBAA is reserved and shall be
        cleared to ‘0’ by software.
        */

        // Initialize scratchpad buffer array if needed
        if (m_maxScratchpadBuffers > 0) {
            uint64_t* scratchpadArray = (uint64_t*)_allocXhciMemory(m_maxScratchpadBuffers * sizeof(uint64_t));
            
            // Create scratchpad pages
            for (uint8_t i = 0; i < m_maxScratchpadBuffers; i++) {
                void* scratchpad = _allocXhciMemory(PAGE_SIZE, XHCI_SCRATCHPAD_BUFFERS_ALIGNMENT, XHCI_SCRATCHPAD_BUFFERS_BOUNDARY);
                uint64_t scratchpadPhysicalBase = (uint64_t)__pa(scratchpad);

                scratchpadArray[i] = scratchpadPhysicalBase;
            }

            uint64_t scratchpadArrayPhysicalBase = (uint64_t)__pa(scratchpadArray);

            // Set the first slot in the DCBAA to point to the scratchpad array
            m_dcbaa[0] = scratchpadArrayPhysicalBase;
        }

        // Set DCBAA pointer in the operational registers
        uint64_t dcbaaPhysicalBase = (uint64_t)__pa(m_dcbaa);

        m_opRegs->dcbaap = dcbaaPhysicalBase;
    }

    void XhciDriver::_mapDeviceMmio(uint64_t pciBarAddress) {
        // Map a conservatively large space for xHCI registers
        for (size_t offset = 0; offset < 0x20000; offset += PAGE_SIZE) {
            void* mmioPage = (void*)(pciBarAddress + offset);
            paging::mapPage(mmioPage, mmioPage, KERNEL_PAGE, PAGE_ATTRIB_CACHE_DISABLED, paging::g_kernelRootPageTable);
        }

        paging::flushTlbAll();

        m_xhcBase = pciBarAddress;
    }

    bool XhciDriver::_resetHostController() {
        // Make sure we clear the Run/Stop bit
        uint32_t usbcmd = m_opRegs->usbcmd;
        usbcmd &= ~XHCI_USBCMD_RUN_STOP;
        m_opRegs->usbcmd = usbcmd;

        // Wait for the HCHalted bit to be set
        uint32_t timeout = 20;
        while (!(m_opRegs->usbsts & XHCI_USBSTS_HCH)) {
            if (--timeout == 0) {
                kprint("XHCI HC did not halt within %ims\n", timeout);
                return false;
            }

            msleep(1);
        }

        // Set the HC Reset bit
        usbcmd = m_opRegs->usbcmd;
        usbcmd |= XHCI_USBCMD_HCRESET;
        m_opRegs->usbcmd = usbcmd;

        // Wait for this bit and CNR bit to clear
        timeout = 100;
        while (
            m_opRegs->usbcmd & XHCI_USBCMD_HCRESET ||
            m_opRegs->usbsts & XHCI_USBSTS_CNR
        ) {
            if (--timeout == 0) {
                kprint("XHCI HC did not reset within %ims\n", timeout);
                return false;
            }

            msleep(1);
        }

        msleep(50);

        // Check the defaults of the operational registers
        if (m_opRegs->usbcmd != 0)
            return false;

        if (m_opRegs->dnctrl != 0)
            return false;

        if (m_opRegs->crcr != 0)
            return false;

        if (m_opRegs->dcbaap != 0)
            return false;

        if (m_opRegs->config != 0)
            return false;

        return true;
    }

    void XhciDriver::_startHostController() {
        uint32_t usbcmd = m_opRegs->usbcmd;
        usbcmd |= XHCI_USBCMD_RUN_STOP;
        usbcmd |= XHCI_USBCMD_INTERRUPTER_ENABLE;
        usbcmd |= XHCI_USBCMD_HOSTSYS_ERROR_ENABLE;

        m_opRegs->usbcmd = usbcmd;

        // Make sure the controller's HCH flag is cleared
        while (m_opRegs->usbsts & XHCI_USBSTS_HCH) {
            msleep(16);
        }
    }

    bool XhciDriver::_resetPort(uint8_t portNum) {
        XhciPortRegisterManager regset = _getPortRegisterSet(portNum);
        XhciPortscRegister portsc;
        regset.readPortscReg(portsc);

        bool isUsb3Port = _isUSB3Port(portNum);

        if (portsc.pp == 0) {
            portsc.pp = 1;
            regset.writePortscReg(portsc);
            msleep(20);
            regset.readPortscReg(portsc);

            if (portsc.pp == 0) {
                kprintWarn("Port %i: Bad Reset\n", portNum);
                return false;
            }
        }

        // Clear connect status change bit by writing a '1' to it
        portsc.csc = 1;
        regset.writePortscReg(portsc);

        // Write to the appropriate reset bit
        if (isUsb3Port) {
            portsc.wpr = 1;
        } else {
            portsc.pr = 1;
        }
        portsc.ped = 0;
        regset.writePortscReg(portsc);

        int timeout = 500;
        while (timeout) {
            regset.readPortscReg(portsc);

            // Detect port reset change bit to be set
            if (isUsb3Port && portsc.wrc) {
                break;
            } else if (!isUsb3Port && portsc.prc) {
                break;
            }

            timeout--;
            msleep(1);
        }

        if (timeout > 0) {
            msleep(3);
            regset.readPortscReg(portsc);

            // Check for the port enable/disable bit
            // to be set and indicate 'enabled' state.
            if (portsc.ped) {
                // Clear connect status change bit by writing a '1' to it
                portsc.csc = 1;
                regset.writePortscReg(portsc);
                return true;
            }
        }

        return false; 
    }

    uint8_t XhciDriver::_requestDeviceSlot() {
        // Small delay period between ringing the
        // doorbell and polling the event ring.
        const uint32_t commandDelay = 10;

        m_commandRing->enqueueEnableSlotTrb();
        m_doorbellManager->ringCommandDoorbell();

        // Let the host controller process the command
        msleep(commandDelay);
        
        // Poll the event ring for the command completion event
        kstl::vector<XhciTrb_t*> events;
        if (m_eventRing->hasUnprocessedEvents()) {
            m_eventRing->dequeueEvents(events);
            _markXhciInterruptCompleted(0);
        }

        XhciCompletionTrb_t* completionTrb = nullptr;
        for (size_t i = 0; i < events.size(); ++i) {
            if (events[i]->trbType == XHCI_TRB_TYPE_CMD_COMPLETION_EVENT) {
                completionTrb = reinterpret_cast<XhciCompletionTrb_t*>(events[i]);
                break;   
            }
        }

        if (!completionTrb || completionTrb->completionCode != 1) {
            return 0;
        }

        return completionTrb->slotId;
    }

    void XhciDriver::_createDeviceContext(uint8_t slotId, uint8_t port, uint8_t portSpeed) {
        // Determine the size of the device context
        // based on the capability register parameters.
        uint64_t deviceContextSize = m_64ByteContextSize ? sizeof(XhciDeviceContext64) : sizeof(XhciDeviceContext32);

        // Allocate a memory block for the device context
        void* ctx = _allocXhciMemory(
            deviceContextSize,
            XHCI_DEVICE_CONTEXT_ALIGNMENT,
            XHCI_DEVICE_CONTEXT_BOUNDARY
        );

        if (!ctx) {
            kprintError("[*] CRITICAL FAILURE: Failed to allocate memory for a device context\n");
            return;
        }

        // Zero out the device context memory by default
        zeromem(ctx, sizeof(deviceContextSize));

        // Insert the device context's physical address
        // into the Device Context Base Addres Array (DCBAA).
        m_dcbaa[slotId] = (uint64_t)__pa(ctx);

        // Determine the size of the input control context
        // based on the capability register parameters.
        uint64_t inputControlContextSize = m_64ByteContextSize ? sizeof(XhciInputControlContext64) : sizeof(XhciInputControlContext32); 

        // Allocate a memory block for the device context
        void* inputControlCtx = _allocXhciMemory(
            inputControlContextSize,
            XHCI_INPUT_CONTROL_CONTEXT_ALIGNMENT,
            XHCI_INPUT_CONTROL_CONTEXT_BOUNDARY
        );

        if (!inputControlCtx) {
            kprintError("[*] CRITICAL FAILURE: Failed to allocate memory for a device input control context\n");
            return;
        }

        // Zero out the device context memory by default
        zeromem(inputControlCtx, sizeof(inputControlContextSize));

        // Calculate the max packet size for the control endpoint context
        uint32_t maxPacketSize = 0;
        switch (portSpeed) {
        case XHCI_USB_SPEED_LOW_SPEED: maxPacketSize = 8; break;
        case XHCI_USB_SPEED_FULL_SPEED: maxPacketSize = 64; break;
        case XHCI_USB_SPEED_HIGH_SPEED: maxPacketSize = 64; break;
        case XHCI_USB_SPEED_SUPER_SPEED: maxPacketSize = 512; break;
        case XHCI_USB_SPEED_SUPER_SPEED_PLUS: maxPacketSize = 512; break;
        default: maxPacketSize = 8; break;
        }

        // Create a transfer ring
        kstl::SharedPtr<XhciTransferRing> transferRing = kstl::SharedPtr<XhciTransferRing>(
            new XhciTransferRing(XHCI_TRANSFER_RING_TRB_COUNT)
        );

        // Set the appropriate fields in the Slot Context
        if (m_64ByteContextSize) {
            // Configure the input context
            XhciInputControlContext64* inputContext = (XhciInputControlContext64*)inputControlCtx;
            inputContext->enableSlotCtx = 1;
            inputContext->enableControlCtx = 1;

            // Copy the device context into the input context buffer
            memcpy(&inputContext->deviceContext, ctx, deviceContextSize);

            XhciDeviceContext64* context = &inputContext->deviceContext;

            // Initialize the slot context
            context->slotContext.ctx32.speed = portSpeed;
            context->slotContext.ctx32.portNum = port + 1; // one-based
            context->slotContext.ctx32.contextEntries = 1;
            context->slotContext.ctx32.interrupterTarget = 0;

            // Initialize the control endpoint context
            context->endpointContext[0].ctx32.epType = 4;
            context->endpointContext[0].ctx32.interval = 0;
            context->endpointContext[0].ctx32.cErr = 3;
            context->endpointContext[0].ctx32.maxPacketSize = maxPacketSize;
            context->endpointContext[0].ctx32.avgTrbLength = 8;
            context->endpointContext[0].ctx32.trDequeuePointer = transferRing->getPhysicalBase() | transferRing->getCycleBit();
        } else {
            // Configure the input context
            XhciInputControlContext32* inputContext = (XhciInputControlContext32*)inputControlCtx;
            inputContext->enableSlotCtx = 1;
            inputContext->enableControlCtx = 1;

            // Copy the device context into the input context buffer
            memcpy(&inputContext->deviceContext, ctx, deviceContextSize);

            XhciDeviceContext32* context = &inputContext->deviceContext;

            // Initialize the slot context
            context->slotContext.speed = portSpeed;
            context->slotContext.portNum = port + 1; // one-based
            context->slotContext.contextEntries = 1;
            context->slotContext.interrupterTarget = 0;

            // Initialize the control endpoint context
            context->endpointContext[0].epType = 4;
            context->endpointContext[0].interval = 0;
            context->endpointContext[0].cErr = 3;
            context->endpointContext[0].maxPacketSize = maxPacketSize;
            context->endpointContext[0].avgTrbLength = 8;
            context->endpointContext[0].trDequeuePointer = transferRing->getPhysicalBase() | transferRing->getCycleBit();
        }

        // Now we can finally use the SET_ADDRESS command to
        // let the host controller initialize the device context.
        XhciTrb_t setAddressCmdTrb = XHCI_CONSTRUCT_CMD_TRB(XHCI_TRB_TYPE_ADDRESS_DEVICE_CMD);
        m_commandRing->enqueue(setAddressCmdTrb);

        // Ring the command doorbell
        m_doorbellManager->ringCommandDoorbell();

        // Small delay period between ringing the
        // doorbell and polling the event ring.
        msleep(10);

        // Poll the event ring for the command completion event
        kstl::vector<XhciTrb_t*> events;
        if (m_eventRing->hasUnprocessedEvents()) {
            m_eventRing->dequeueEvents(events);
            _markXhciInterruptCompleted(0);
        }

        XhciCompletionTrb_t* completionTrb = nullptr;
        for (size_t i = 0; i < events.size(); ++i) {
            if (events[i]->trbType == XHCI_TRB_TYPE_CMD_COMPLETION_EVENT) {
                completionTrb = reinterpret_cast<XhciCompletionTrb_t*>(events[i]);
                break;   
            }
        }

        if (!completionTrb) {
            kprintError("[*] Host controller didn't receive SET_ADDRESS command to initialize the device context!\n");
            return;
        }

        if (completionTrb->completionCode != 1) {
            kprintError("[*] Host controller failed to process SET_ADDRESS command to initialize the device context!\n");
            kprintError("        Completion Code: %s\n", trbCompletionCodeToString(completionTrb->completionCode));
            return;
        }

        kprintInfo("[*] Check your DCBAA for correctness :) good job!\n");
    }

    void XhciDriver::_markXhciInterruptCompleted(uint8_t interrupter) {
        // Get the interrupter registers
        auto interrupterRegs = m_runtimeRegisterManager->getInterrupterRegisters(interrupter);

        // Clear the interrupt pending bit in the primary interrupter
        interrupterRegs->iman |= ~XHCI_IMAN_INTERRUPT_PENDING;

        // Clear the interrupt pending bit in USBSTS
        m_opRegs->usbsts |= ~XHCI_USBSTS_EINT;
    }

    void XhciDriver::_processEventRingTrb(XhciTrb_t* trb) {
        if (trb->trbType == XHCI_TRB_TYPE_CMD_COMPLETION_EVENT) {
            // auto completionTrb = reinterpret_cast<XhciCompletionTrb_t*>(trb);
            // XhciTrb_t* commandTrb = (XhciTrb_t*)__va((void*)completionTrb->commandTrbPointer);

            // if (commandTrb->trbType == XHCI_TRB_TYPE_ENABLE_SLOT_CMD) {
            //     kprintInfo("Found Completion TRB: 'Enable Slot Command'\n");
            // } else if (commandTrb->trbType == XHCI_TRB_TYPE_NOOP_CMD) {
            //     kprintInfo("Found Completion TRB: 'No-Op Command'\n");
            // } else if (commandTrb->trbType == XHCI_TRB_TYPE_RESET_ENDPOINT_CMD) {
            //     kprintInfo("Found Completion TRB: 'Reset Endpoint Command'\n");
            // } else {
            //     kprintInfo("Found Completion TRB: %i\n", commandTrb->trbType);
            // }
        } else if (trb->trbType == XHCI_TRB_TYPE_PORT_STATUS_CHANGE_EVENT) {
            auto pscTrb = reinterpret_cast<XhciPortStatusChangeTrb_t*>(trb);
            uint8_t port = pscTrb->portId - 1; // TRB's portId index is 1-based

            auto portRegisterSet = _getPortRegisterSet(port);
            XhciPortscRegister portsc;
            portRegisterSet.readPortscReg(portsc);

            if (portsc.csc) {
                if (portsc.ccs) {
                    _handleDeviceConnected(port);
                } else {
                    _handleDeviceDiconnected(port);
                }
            }

            if (portsc.csc) portsc.csc = 1;
            if (portsc.pec) portsc.pec = 1;
            if (portsc.wrc) portsc.wrc = 1;
            if (portsc.occ) portsc.occ = 1;
            if (portsc.prc) portsc.prc = 1;
            if (portsc.cec) portsc.cec = 1;

            portRegisterSet.writePortscReg(portsc);
        } else if (trb->trbType == XHCI_TRB_TYPE_HOST_CONTROLLER_EVENT) {
            // kprintInfo("Found Host Controller Event TRB\n");
        }
    }

    void XhciDriver::_handleDeviceConnected(uint8_t port) {
        auto portRegisterSet = _getPortRegisterSet(port);
        XhciPortscRegister portsc;
        portRegisterSet.readPortscReg(portsc);

        kprintInfo("Port State Change Event on port %i: ", port);
        kprint("%s device ATTACHED with speed ", _isUSB3Port(port) ? "USB3" : "USB2");

        switch (portsc.portSpeed) {
        case XHCI_USB_SPEED_FULL_SPEED: kprint("Full Speed (12 MB/s - USB2.0)\n"); break;
        case XHCI_USB_SPEED_LOW_SPEED: kprint("Low Speed (1.5 Mb/s - USB 2.0)\n"); break;
        case XHCI_USB_SPEED_HIGH_SPEED: kprint("High Speed (480 Mb/s - USB 2.0)\n"); break;
        case XHCI_USB_SPEED_SUPER_SPEED: kprint("Super Speed (5 Gb/s - USB3.0)\n"); break;
        case XHCI_USB_SPEED_SUPER_SPEED_PLUS: kprint("Super Speed Plus (10 Gb/s - USB 3.1)\n"); break;
        default: kprint("Undefined\n"); break;
        }

        uint8_t deviceSlot = _requestDeviceSlot();
        if (deviceSlot == 0) {
            kprintError("[*] Failed to enable Device Slot for port %i\n", port);
            return;
        }
        kprintInfo("Received Device Slot ID %i\n", deviceSlot);

        _createDeviceContext(deviceSlot, port, (uint8_t)portsc.portSpeed);

        kprint("\n");
    }

    void XhciDriver::_handleDeviceDiconnected(uint8_t port) {
        kprintInfo("Port State Change Event on port %i: ", port);
        kprint("%s device DETACHED\n", _isUSB3Port(port) ? "USB3" : "USB2");
    }
} // namespace drivers
