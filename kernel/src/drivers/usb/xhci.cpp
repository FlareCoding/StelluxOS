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

        const uint64_t ringSize = XHCI_COMMAND_RING_TRB_COUNT * sizeof(XhciTrb_t);

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

    void XhciEventRing::processEvents() {
        // Process each event TRB
        while (hasUnprocessedEvents()) {
            int slot = (int)m_dequeuePtr;
            auto eventTrb = _dequeueTrb();
            if (!eventTrb) break;

            if (eventTrb->trbType == XHCI_TRB_TYPE_CMD_COMPLETION_EVENT) {
                auto completionTrb = reinterpret_cast<XhciCompletionTrb_t*>(eventTrb);
                XhciTrb_t* commandTrb = (XhciTrb_t*)__va((void*)completionTrb->commandTrbPointer);

                if (commandTrb->trbType == XHCI_TRB_TYPE_ENABLE_SLOT_CMD) {
                    kprintInfo("Found Completion TRB at slot %i: 'Enable Slot Command'\n", slot);
                } else if (commandTrb->trbType == XHCI_TRB_TYPE_NOOP_CMD) {
                    kprintInfo("Found Completion TRB at slot %i: 'No-Op Command'\n", slot);
                } else if (commandTrb->trbType == XHCI_TRB_TYPE_RESET_ENDPOINT_CMD) {
                    kprintInfo("Found Completion TRB at slot %i: 'Reset Endpoint Command'\n", slot);
                } else {
                    kprintInfo("Found Completion TRB at slot %i: %i\n", slot, commandTrb->trbType);
                }
            } else if (eventTrb->trbType == XHCI_TRB_TYPE_PORT_STATUS_CHANGE_EVENT) {
                kprintInfo("Found Port Status Change Event TRB at slot %i\n", slot);
            } else if (eventTrb->trbType == XHCI_TRB_TYPE_HOST_CONTROLLER_EVENT) {
                kprintInfo("Found Host Controller Event TRB at slot %i\n", slot);
            }
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

        // Debug log
        _logOperationalRegisters();
        _logUsbsts();

        // Reset the ports
        for (uint8_t i = 0; i < m_maxPorts; i++) {
            if (_resetPort(i)) {
                kprintInfo("[*] Successfully reset %s port %i\n", _isUSB3Port(i) ? "USB3" : "USB2", i);
            } else {
                kprintWarn("[*] Failed to reset %s port %i\n", _isUSB3Port(i) ? "USB3" : "USB2", i);
            }
        }
        kprint("\n");

        for (uint8_t i = 0; i < m_maxPorts; i++) {
            XhciPortRegisterManager regset = _getPortRegisterSet(i);
            XhciPortscRegister portsc;
            regset.readPortscReg(portsc);

            bool isUsb3Port = _isUSB3Port(i);

            if (portsc.ccs) {
                kprint("%s device connected on port %i with speed ", isUsb3Port ? "USB3" : "USB2", i);

                switch (portsc.portSpeed) {
                case XHCI_USB_SPEED_FULL_SPEED: kprint("Full Speed (12 MB/s - USB2.0)\n"); break;
                case XHCI_USB_SPEED_LOW_SPEED: kprint("Low Speed (1.5 Mb/s - USB 2.0)\n"); break;
                case XHCI_USB_SPEED_HIGH_SPEED: kprint("High Speed (480 Mb/s - USB 2.0)\n"); break;
                case XHCI_USB_SPEED_SUPER_SPEED: kprint("Super Speed (5 Gb/s - USB3.0)\n"); break;
                case XHCI_USB_SPEED_SUPER_SPEED_PLUS: kprint("Super Speed Plus (10 Gb/s - USB 3.1)\n"); break;
                default: kprint("Undefined\n"); break;
                }
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

        uint64_t* dcbaaVirtualBase = (uint64_t*)_allocXhciMemory(dcbaaSize, XHCI_DEVICE_CONTEXT_ALIGNMENT, XHCI_DEVICE_CONTEXT_BOUNDARY);
        zeromem(dcbaaVirtualBase, dcbaaSize);

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
            dcbaaVirtualBase[0] = scratchpadArrayPhysicalBase;
        }

        // Set DCBAA pointer in the operational registers
        uint64_t dcbaaPhysicalBase = (uint64_t)__pa(dcbaaVirtualBase);

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
} // namespace drivers
