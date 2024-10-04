#include "xhci.h"
#include <paging/page.h>
#include <paging/phys_addr_translation.h>
#include <paging/tlb.h>
#include <memory/kmemory.h>
#include <time/ktime.h>
#include <arch/x86/apic.h>
#include <arch/x86/ioapic.h>
#include <arch/x86/cpuid.h>
#include <interrupts/interrupts.h>
#include <sched/sched.h>
#include <kstring.h>
#include <kprint.h>

bool g_singletonInitialized = false;

int XhciDriver::driverInit(PciDeviceInfo& pciInfo, uint8_t irqVector) {
    if (g_singletonInitialized) {
        kprintWarn("[XHCI] Another instance of the controller driver is already running\n");
    }

    kprint("[XHCI] Initializing xHci Driver 3.0\n\n");

    m_xhcBase = xhciMapMmio(pciInfo.barAddress);

    // Parse the read-only capability register space
    _parseCapabilityRegisters();
    _logCapabilityRegisters();

    // Parse the extended capabilities
    _parseExtendedCapabilityRegisters();

    // Reset the controller
    if (!_resetHostController()) {
        return DEVICE_INIT_FAILURE;
    }

    // Configure the controller's register spaces
    _configureOperationalRegisters();
    _configureRuntimeRegisters();

    // Register the IRQ handler
    if (irqVector != 0) {
        registerIrqHandler(irqVector, reinterpret_cast<IrqHandler_t>(xhciIrqHandler), false, static_cast<void*>(this));
        kprint("Registered xhci handler at IRQ%i\n\n", irqVector - IRQ0);
    }

    // At this point the controller is all setup so we can start it
    _startHostController();

    // Perform an initial port reset for each port
    for (uint8_t i = 0; i < m_maxPorts; i++) {
        _resetPort(i);
    }

    // This code is just a prototype right now and is by no
    // means safe and has critical synchronization issues.
    while (true) {
        msleep(20);
        if (m_portStatusChangeEvents.empty()) {
            continue; 
        }

        for (size_t i = 0; i < m_portStatusChangeEvents.size(); i++) {
            uint8_t port = m_portStatusChangeEvents[i]->portId;
            uint8_t portRegIdx = port - 1;
            
            XhciPortRegisterManager regman = _getPortRegisterSet(portRegIdx);
            XhciPortscRegister reg;
            regman.readPortscReg(reg);

            if (reg.ccs) {
                kprintInfo("[XHCI] Device connected on port %i - %s\n", port, _usbSpeedToString(reg.portSpeed));

                // Setup the newly connected device
                _setupDevice(portRegIdx);

            } else {
                kprintInfo("[XHCI] Device disconnected from port %i\n", port);
            }
        }

        m_portStatusChangeEvents.clear();
    }

    return DEVICE_INIT_SUCCESS;
}

void XhciDriver::logUsbsts() {
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

irqreturn_t XhciDriver::xhciIrqHandler(void*, XhciDriver* driver) {
    driver->_processEvents();
    driver->_acknowledgeIrq(0);

    Apic::getLocalApic()->completeIrq();
    return IRQ_HANDLED;
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

uint8_t XhciDriver::_getPortSpeed(uint8_t port) {
    auto portRegisterSet = _getPortRegisterSet(port);
    XhciPortscRegister portsc;
    portRegisterSet.readPortscReg(portsc);

    return static_cast<uint8_t>(portsc.portSpeed);
}

const char* XhciDriver::_usbSpeedToString(uint8_t speed) {
    static const char* speedString[7] = {
        "Invalid",
        "Full Speed (12 MB/s - USB2.0)",
        "Low Speed (1.5 Mb/s - USB 2.0)",
        "High Speed (480 Mb/s - USB 2.0)",
        "Super Speed (5 Gb/s - USB3.0)",
        "Super Speed Plus (10 Gb/s - USB 3.1)",
        "Undefined"
    };

    return speedString[speed];
}

void XhciDriver::_configureRuntimeRegisters() {
    // Get the primary interrupter registers
    auto interrupterRegs = m_runtimeRegisterManager->getInterrupterRegisters(0);
    if (!interrupterRegs) {
        kprintError("[*] Failed to retrieve interrupter register set when setting up the event ring!");
        return;
    }

    // Enable interrupts
    interrupterRegs->iman |= XHCI_IMAN_INTERRUPT_ENABLE;

    // Setup the event ring and write to interrupter
    // registers to set ERSTSZ, ERSDP, and ERSTBA.
    m_eventRing = kstl::SharedPtr<XhciEventRing>(
        new XhciEventRing(XHCI_EVENT_RING_TRB_COUNT, interrupterRegs)
    );

    // Clear any pending interrupts for primary interrupter
    _acknowledgeIrq(0);
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

    m_dcbaa = (uint64_t*)allocXhciMemory(dcbaaSize, XHCI_DEVICE_CONTEXT_ALIGNMENT, XHCI_DEVICE_CONTEXT_BOUNDARY);
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
        uint64_t* scratchpadArray = (uint64_t*)allocXhciMemory(m_maxScratchpadBuffers * sizeof(uint64_t));
        
        // Create scratchpad pages
        for (uint8_t i = 0; i < m_maxScratchpadBuffers; i++) {
            void* scratchpad = allocXhciMemory(PAGE_SIZE, XHCI_SCRATCHPAD_BUFFERS_ALIGNMENT, XHCI_SCRATCHPAD_BUFFERS_BOUNDARY);
            uint64_t scratchpadPhysicalBase = physbase(scratchpad);

            scratchpadArray[i] = scratchpadPhysicalBase;
        }

        uint64_t scratchpadArrayPhysicalBase = physbase(scratchpadArray);

        // Set the first slot in the DCBAA to point to the scratchpad array
        m_dcbaa[0] = scratchpadArrayPhysicalBase;
    }

    // Set DCBAA pointer in the operational registers
    uint64_t dcbaaPhysicalBase = physbase(m_dcbaa);

    m_opRegs->dcbaap = dcbaaPhysicalBase;
}

void XhciDriver::_createDeviceContext(uint8_t slotId) {
    // Determine the size of the device context
    // based on the capability register parameters.
    uint64_t deviceContextSize = m_64ByteContextSize ? sizeof(XhciDeviceContext64) : sizeof(XhciDeviceContext32);

    // Allocate a memory block for the device context
    void* ctx = allocXhciMemory(
        deviceContextSize,
        XHCI_DEVICE_CONTEXT_ALIGNMENT,
        XHCI_DEVICE_CONTEXT_BOUNDARY
    );

    if (!ctx) {
        kprintError("[*] CRITICAL FAILURE: Failed to allocate memory for a device context\n");
        return;
    }

    // Insert the device context's physical address
    // into the Device Context Base Addres Array (DCBAA).
    m_dcbaa[slotId] = physbase(ctx);
}

XhciCommandCompletionTrb_t* XhciDriver::_sendCommand(XhciTrb_t* trb, uint32_t timeoutMs) {
    // Enqueue the TRB
    m_commandRing->enqueue(trb);

    // Ring the command doorbell
    m_doorbellManager->ringCommandDoorbell();

    // Let the host controller process the command
    uint64_t sleepPassed = 0;
    while (!m_commandIrqCompleted) {
        usleep(10);
        sleepPassed += 10;

        if (sleepPassed > timeoutMs * 1000) {
            break;
        }
    }

    XhciCommandCompletionTrb_t* completionTrb =
        m_commandCompletionEvents.size() ? m_commandCompletionEvents[0] : nullptr;

    // Reset the irq flag and clear out the command completion event queue
    m_commandCompletionEvents.clear();
    m_commandIrqCompleted = 0;

    if (!completionTrb) {
        kprintError("[*] Failed to find completion TRB for command %i\n", trb->trbType);
        return nullptr;
    }

    if (completionTrb->completionCode != XHCI_TRB_COMPLETION_CODE_SUCCESS) {
        kprintError("[*] Command TRB failed with error: %s\n", trbCompletionCodeToString(completionTrb->completionCode));
        return nullptr;
    }

    return completionTrb;
}

XhciTransferCompletionTrb_t* XhciDriver::_startControlEndpointTransfer(XhciTransferRing* transferRing) {
    // Ring the endpoint's doorbell
    m_doorbellManager->ringControlEndpointDoorbell(transferRing->getDoorbellId());

    // Let the host controller process the command
    const uint64_t timeoutMs = 400; 
    uint64_t sleepPassed = 0;
    while (!m_transferIrqCompleted) {
        usleep(10);
        sleepPassed += 10;

        if (sleepPassed > timeoutMs * 1000) {
            break;
        }
    }

    XhciTransferCompletionTrb_t* completionTrb =
        m_transferCompletionEvents.size() ? m_transferCompletionEvents[0] : nullptr;

    // Reset the irq flag and clear out the command completion event queue
    m_transferCompletionEvents.clear();
    m_transferIrqCompleted = 0;

    if (!completionTrb) {
        kprintError("[*] Failed to find transfer completion TRB\n");
        return nullptr;
    }

    if (completionTrb->completionCode != XHCI_TRB_COMPLETION_CODE_SUCCESS) {
        kprintError("[*] Transfer TRB failed with error: %s\n", trbCompletionCodeToString(completionTrb->completionCode));
        return nullptr;
    }

    return completionTrb;
}

uint16_t XhciDriver::_getMaxInitialPacketSize(uint8_t portSpeed) {
    // Calculate initial max packet size for the set device command
    uint16_t initialMaxPacketSize = 0;
    switch (portSpeed) {
    case XHCI_USB_SPEED_LOW_SPEED: initialMaxPacketSize = 8; break;

    case XHCI_USB_SPEED_FULL_SPEED:
    case XHCI_USB_SPEED_HIGH_SPEED: initialMaxPacketSize = 64; break;

    case XHCI_USB_SPEED_SUPER_SPEED:
    case XHCI_USB_SPEED_SUPER_SPEED_PLUS:
    default: initialMaxPacketSize = 512; break;
    }

    return initialMaxPacketSize;
}

void XhciDriver::_processEvents() {
    // Poll the event ring for the command completion event
    kstl::vector<XhciTrb_t*> events;
    if (m_eventRing->hasUnprocessedEvents()) {
        m_eventRing->dequeueEvents(events);
    }

    uint8_t portChangeEventStatus = 0;
    uint8_t commandCompletionStatus = 0;
    uint8_t transferCompletionStatus = 0;

    for (size_t i = 0; i < events.size(); i++) {
        XhciTrb_t* event = events[i];
        switch (event->trbType) {
        case XHCI_TRB_TYPE_PORT_STATUS_CHANGE_EVENT: {
            portChangeEventStatus = 1;
            m_portStatusChangeEvents.pushBack((XhciPortStatusChangeTrb_t*)event);
            break;
        }
        case XHCI_TRB_TYPE_CMD_COMPLETION_EVENT: {
            commandCompletionStatus = 1;
            m_commandCompletionEvents.pushBack((XhciCommandCompletionTrb_t*)event);
            break;
        }
        case XHCI_TRB_TYPE_TRANSFER_EVENT: {
            transferCompletionStatus = 1;
            m_transferCompletionEvents.pushBack((XhciTransferCompletionTrb_t*)event);
            break;
        }
        default: {
            kprint("Received an event! Please parse...\n");
            break;
        }
        }
    }

    m_commandIrqCompleted = commandCompletionStatus;
    m_transferIrqCompleted = transferCompletionStatus;
}

void XhciDriver::_acknowledgeIrq(uint8_t interrupter) {
    // Get the interrupter registers
    XhciInterrupterRegisters* interrupterRegs =
        m_runtimeRegisterManager->getInterrupterRegisters(interrupter);

    // Read the current value of IMAN
    uint32_t iman = interrupterRegs->iman;

    // Set the IP bit to '1' to clear it, preserve other bits including IE
    uint32_t iman_write = iman | XHCI_IMAN_INTERRUPT_PENDING;

    // Write back to IMAN
    interrupterRegs->iman = iman_write;

    // Clear the EINT bit in USBSTS by writing '1' to it
    m_opRegs->usbsts = XHCI_USBSTS_EINT;
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

    int timeout = 100;
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

uint8_t XhciDriver::_enableDeviceSlot() {
    XhciTrb_t enableSlotTrb = XHCI_CONSTRUCT_CMD_TRB(XHCI_TRB_TYPE_ENABLE_SLOT_CMD);
    auto completionTrb = _sendCommand(&enableSlotTrb);

    if (!completionTrb) {
        return 0;
    }

    return completionTrb->slotId;
}

void XhciDriver::_configureDeviceInputContext(XhciDevice* device, uint16_t maxPacketSize) {
    XhciInputControlContext32* inputControlContext = device->getInputControlContext(m_64ByteContextSize);
    XhciSlotContext32* slotContext = device->getInputSlotContext(m_64ByteContextSize);
    XhciEndpointContext32* controlEndpointContext = device->getInputControlEndpointContext(m_64ByteContextSize);

    // Enable slot and control endpoint contexts
    inputControlContext->addFlags = (1 << 0) | (1 << 1);
    inputControlContext->dropFlags = 0;

    // Configure the slot context
    slotContext->contextEntries = 1;
    slotContext->speed = device->speed;
    slotContext->rootHubPortNum = device->portNumber;
    slotContext->routeString = 0;
    slotContext->interrupterTarget = 0;

    // Configure the control endpoint context
    controlEndpointContext->endpointState = XHCI_ENDPOINT_STATE_DISABLED;
    controlEndpointContext->endpointType = XHCI_ENDPOINT_TYPE_CONTROL;
    controlEndpointContext->interval = 0;
    controlEndpointContext->errorCount = 3;
    controlEndpointContext->maxPacketSize = maxPacketSize;
    controlEndpointContext->transferRingDequeuePtr = device->getControlEndpointTransferRing()->getPhysicalDequeuePointerBase();
    controlEndpointContext->dcs = device->getControlEndpointTransferRing()->getCycleBit();
    controlEndpointContext->maxEsitPayloadLo = 0;
    controlEndpointContext->maxEsitPayloadHi = 0;
    controlEndpointContext->averageTrbLength = 8;
}

void XhciDriver::_configureDeviceInterruptEndpoint(XhciDevice* device, UsbEndpointDescriptor* epDesc) {
    XhciInputControlContext32* inputControlContext = device->getInputControlContext(m_64ByteContextSize);
    XhciSlotContext32* slotContext = device->getInputSlotContext(m_64ByteContextSize);

    uint8_t endpointNumber = epDesc->bEndpointAddress & 0x0F;
    kprint("endpointnumber: %i\n", endpointNumber);
    uint8_t endpointDirectionIn = (epDesc->bEndpointAddress & 0x80) ? 1 : 0;
    uint8_t endpointId = (endpointNumber * 2) + endpointDirectionIn;
    kprint("endpointId: %i\n", endpointId);

    // Enable the input control context flags
    inputControlContext->addFlags = (1 << endpointId) | (1 << 0);
    if (endpointId > slotContext->contextEntries) {
        slotContext->contextEntries = endpointId;
    }

    // Configure the endpoint context
    XhciEndpointContext32* interruptEndpointContext = device->getInputEndpointContext(m_64ByteContextSize, endpointId);
    zeromem(interruptEndpointContext, sizeof(XhciEndpointContext32));
    interruptEndpointContext->endpointState = XHCI_ENDPOINT_STATE_DISABLED;
    interruptEndpointContext->endpointType = XHCI_ENDPOINT_TYPE_INTERRUPT_IN;
    interruptEndpointContext->maxPacketSize = epDesc->wMaxPacketSize;
    interruptEndpointContext->errorCount = 3;
    interruptEndpointContext->maxBurstSize = 0;
    interruptEndpointContext->averageTrbLength = 8;
    interruptEndpointContext->transferRingDequeuePtr = device->getInterruptInEndpointTransferRing()->getPhysicalDequeuePointerBase();
    interruptEndpointContext->dcs = device->getInterruptInEndpointTransferRing()->getCycleBit();

    if (device->speed == XHCI_USB_SPEED_HIGH_SPEED || device->speed == XHCI_USB_SPEED_SUPER_SPEED) {
        interruptEndpointContext->interval = epDesc->bInterval - 1;
    } else {
        interruptEndpointContext->interval = epDesc->bInterval;
    }

    kprint("transferRingDequeuePtr: 0x%llx\n", interruptEndpointContext->transferRingDequeuePtr);
    const int intervalInMs = ((2 << (interruptEndpointContext->interval - 1)) * 125);
    kprint("interval: %i  (%i us / %i ms)\n", interruptEndpointContext->interval, intervalInMs, intervalInMs / 1000);
}

void XhciDriver::_setupDevice(uint8_t port) {
    XhciDevice* device = new XhciDevice();
    device->portRegSet = port;
    device->portNumber = port + 1;
    device->speed = _getPortSpeed(port);

    // Calculate the initial max packet size based on device speed
    uint16_t maxPacketSize = _getMaxInitialPacketSize(device->speed);

    kprintInfo("Setting up %s device on port %i (portReg:%i)\n", _usbSpeedToString(device->speed), device->portNumber, port);

    device->slotId = _enableDeviceSlot();
    if (!device->slotId) {
        kprintError("[XHCI] Failed to setup device\n");
        delete device;
        return;
    }

    kprintInfo("Device slotId: %i\n", device->slotId);
    _createDeviceContext(device->slotId);

    // Allocate space for a command input context and transfer ring
    device->allocateInputContext(m_64ByteContextSize);
    device->allocateControlEndpointTransferRing();

    // Configure the command input context
    _configureDeviceInputContext(device, maxPacketSize);

    // First address device with BSR=1, essentially blocking the SET_ADDRESS request,
    // but still enables the control endpoint which we can use to get the device descriptor.
    // Some legacy devices require their desciptor to be read before sending them a SET_ADDRESS command.
    if (!_addressDevice(device, true)) {
        kprintError("[XHCI] Failed to setup device\n");
        return;
    }

    UsbDeviceDescriptor* deviceDescriptor = new UsbDeviceDescriptor();
    if (!_getDeviceDescriptor(device, deviceDescriptor, 8)) {
        kprintError("[XHCI] Failed to get device descriptor\n");
        return;
    }

    // Reset the port again
    //_resetPort(device->portRegSet);

    // Update the device input context
    _configureDeviceInputContext(device, deviceDescriptor->bMaxPacketSize0);

    // If the read max device packet size is different
    // from the initially calculated one, update it.
    if (deviceDescriptor->bMaxPacketSize0 != maxPacketSize) {
        // Update max packet size with the value from the device descriptor
        maxPacketSize = deviceDescriptor->bMaxPacketSize0;

        // MUST SEND AN EVALUATE_CONTEXT CMD HERE
    }

    // Send the address device command again with BSR=0 this time
    _addressDevice(device, false);

    // Copy the output device context into the device's input context
    device->copyOutputDeviceContextToInputDeviceContext(m_64ByteContextSize, (void*)m_dcbaa[device->slotId]);

    // Read the full device descriptor
    if (!_getDeviceDescriptor(device, deviceDescriptor, deviceDescriptor->header.bLength)) {
        kprintError("[XHCI] Failed to get full device descriptor\n");
        return;
    }

    UsbStringLanguageDescriptor stringLanguageDescriptor;
    if (!_getStringLanguageDescriptor(device, &stringLanguageDescriptor)) {
        return;
    }

    // Get the language ID
    uint16_t langId = stringLanguageDescriptor.langIds[0];

    // Get metadata and information about the device
    UsbStringDescriptor* productName = new UsbStringDescriptor();
    _getStringDescriptor(device, deviceDescriptor->iProduct, langId, productName);

    UsbStringDescriptor* manufacturerName = new UsbStringDescriptor();
    _getStringDescriptor(device, deviceDescriptor->iManufacturer, langId, manufacturerName);

    UsbStringDescriptor* serialNumberString = new UsbStringDescriptor();
    _getStringDescriptor(device, deviceDescriptor->iSerialNumber, langId, serialNumberString);

    char product[255] = { 0 };
    char manufacturer[255] = { 0 };
    char serialNumber[255] = { 0 };

    convertUnicodeToNarrowString(productName->unicodeString, product);
    convertUnicodeToNarrowString(manufacturerName->unicodeString, manufacturer);
    convertUnicodeToNarrowString(serialNumberString->unicodeString, serialNumber);

    UsbConfigurationDescriptor* configurationDescriptor = new UsbConfigurationDescriptor();
    if (!_getConfigurationDescriptor(device, configurationDescriptor)) {
        return;
    }
    
    UsbInterfaceDescriptor* iface = nullptr;
    UsbEndpointDescriptor* epDescriptor = nullptr;

    uint8_t* buffer = configurationDescriptor->data;
    uint16_t totalLength = configurationDescriptor->wTotalLength - configurationDescriptor->header.bLength;
    uint16_t index = 0;

    while (index < totalLength) {
        UsbDescriptorHeader* header = (UsbDescriptorHeader*)&buffer[index];

        switch (header->bDescriptorType) {
            case USB_DESCRIPTOR_INTERFACE:
                iface = (UsbInterfaceDescriptor*)header;
                break;
            case USB_DESCRIPTOR_HID:
                // Process HID Descriptor
                // ...
                break;
            case USB_DESCRIPTOR_ENDPOINT:
                epDescriptor = (UsbEndpointDescriptor*)header;
                break;
            default: break;
        }

        index += header->bLength;
    }

    if (!iface || !epDescriptor) {
        kprintError("[XHCI] Failed to find interface or endpoint descriptor\n");
        return;
    }

    kprint("---- USB Device Info ----\n");
    kprint("  Product Name    : %s\n", product);
    kprint("  Manufacturer    : %s\n", manufacturer);
    kprint("  Serial Number   : %s\n", serialNumber);
    kprint("  Configuration   :\n");
    kprint("      wTotalLength        - %i\n", configurationDescriptor->wTotalLength);
    kprint("      bNumInterfaces      - %i\n", configurationDescriptor->bNumInterfaces);
    kprint("      bConfigurationValue - %i\n", configurationDescriptor->bConfigurationValue);
    kprint("      iConfiguration      - %i\n", configurationDescriptor->iConfiguration);
    kprint("      bmAttributes        - %i\n", configurationDescriptor->bmAttributes);
    kprint("      bMaxPower           - %i milliamps\n", configurationDescriptor->bMaxPower);
    kprint("      ------ Interface 1 ------\n");
    kprint("        type    - %i\n", iface->header.bDescriptorType);
    kprint("        bInterfaceNumber    - %i\n", iface->bInterfaceNumber);
    kprint("        bAlternateSetting   - %i\n", iface->bAlternateSetting);
    kprint("        bNumEndpoints       - %i\n", iface->bNumEndpoints);
    kprint("        bInterfaceClass     - %i\n", iface->bInterfaceClass);
    kprint("        bInterfaceSubClass  - %i\n", iface->bInterfaceSubClass);
    kprint("        bInterfaceProtocol  - %i\n", iface->bInterfaceProtocol);
    kprint("        iInterface          - %i\n", iface->iInterface);
    kprint("        ------ Endpoint 1 ------\n");
    kprint("          bEndpointAddress  - %x\n", epDescriptor->bEndpointAddress);
    kprint("          bmAttributes      - %i\n", epDescriptor->bmAttributes);
    kprint("          wMaxPacketSize    - %i\n", epDescriptor->wMaxPacketSize);
    kprint("          bInterval         - %i\n", epDescriptor->bInterval);
    kprint("\n");

    // Allocate a transfer ring for the interrupt endpoint
    device->allocateInterruptInEndpointTransferRing();

    // Re-configure the input context to enable the interrupt endpoint
    _configureDeviceInterruptEndpoint(device, epDescriptor);

    // Configure the endpoint that we got from the ep descriptor
    if (!_configureEndpoint(device)) {
        return;
    }

    device->copyOutputDeviceContextToInputDeviceContext(m_64ByteContextSize, (void*)m_dcbaa[device->slotId]);

    // Sanity-check the actual device context entry in DCBAA
    XhciDeviceContext32* deviceContext = &virtbase(device->getInputContextPhysicalBase(), XhciInputContext32)->deviceContext;

    kprint("    DeviceContext[slotId=%i] address:0x%i slotState:%s epSate:%s maxPacketSize:%i\n",
        device->slotId, deviceContext->slotContext.deviceAddress,
        xhciSlotStateToString(deviceContext->slotContext.slotState),
        xhciEndpointStateToString(deviceContext->ep[1].endpointState),
        deviceContext->ep[1].maxPacketSize
    );

    // Set device configuration
    if (!_setDeviceConfiguration(device, configurationDescriptor->bConfigurationValue)) {
        return;
    }

    // Set BOOT protocol
    const uint8_t bootProtocol = 0;
    if (!_setProtocol(device, iface->bInterfaceNumber, bootProtocol)) {
        return;
    }

    kprint("Testing keyboard...\n");
    _testKeyboardCommunication(device);
}

bool XhciDriver::_addressDevice(XhciDevice* device, bool bsr) {
    // Construct the Address Device TRB
    XhciAddressDeviceCommandTrb_t addressDeviceTrb;
    zeromem(&addressDeviceTrb, sizeof(XhciAddressDeviceCommandTrb_t));
    addressDeviceTrb.trbType = XHCI_TRB_TYPE_ADDRESS_DEVICE_CMD;
    addressDeviceTrb.inputContextPhysicalBase = device->getInputContextPhysicalBase();
    addressDeviceTrb.bsr = bsr ? 1 : 0;
    addressDeviceTrb.slotId = device->slotId;

    // Send the Address Device command
    XhciCommandCompletionTrb_t* completionTrb = _sendCommand((XhciTrb_t*)&addressDeviceTrb, 200);
    if (!completionTrb) {
        kprintError("[*] Failed to address device with BSR=%i\n", (int)bsr);
        return false;
    }

    if (m_64ByteContextSize) {
        // Sanity-check the actual device context entry in DCBAA
        XhciDeviceContext64* deviceContext = virtbase(m_dcbaa[device->slotId], XhciDeviceContext64);

        kprint("    DeviceContext[slotId=%i] address:0x%llx slotState:%s epSate:%s maxPacketSize:%i\n",
            device->slotId, deviceContext->slotContext.deviceAddress,
            xhciSlotStateToString(deviceContext->slotContext.slotState),
            xhciEndpointStateToString(deviceContext->controlEndpointContext.endpointState),
            deviceContext->controlEndpointContext.maxPacketSize
        );
    } else {
        // Sanity-check the actual device context entry in DCBAA
        XhciDeviceContext32* deviceContext = virtbase(m_dcbaa[device->slotId], XhciDeviceContext32);

        kprint("    DeviceContext[slotId=%i] address:0x%llx slotState:%s epSate:%s maxPacketSize:%i\n",
            device->slotId, deviceContext->slotContext.deviceAddress,
            xhciSlotStateToString(deviceContext->slotContext.slotState),
            xhciEndpointStateToString(deviceContext->controlEndpointContext.endpointState),
            deviceContext->controlEndpointContext.maxPacketSize
        );
    }

    return true;
}

bool XhciDriver::_configureEndpoint(XhciDevice* device) {
    XhciConfigureEndpointCommandTrb_t configureEndpointTrb;
    zeromem(&configureEndpointTrb, sizeof(XhciConfigureEndpointCommandTrb_t));
    configureEndpointTrb.trbType = XHCI_TRB_TYPE_CONFIGURE_ENDPOINT_CMD;
    configureEndpointTrb.inputContextPhysicalBase = device->getInputContextPhysicalBase();
    configureEndpointTrb.slotId = device->slotId;

    // Send the Configure Endpoint command
    XhciCommandCompletionTrb_t* completionTrb = _sendCommand((XhciTrb_t*)&configureEndpointTrb, 200);
    if (!completionTrb) {
        kprintError("[*] Failed to send Configure Endpoint command\n");
        return false;
    }

    // Check the completion code
    if (completionTrb->completionCode != XHCI_TRB_COMPLETION_CODE_SUCCESS) {
        kprintError("[*] Evaluate Context command failed with completion code: %s\n",
                    trbCompletionCodeToString(completionTrb->completionCode));
        return false;
    }

    return true;
}

bool XhciDriver::_evaluateContext(XhciDevice* device) {
    // Construct the Evaluate Context Command TRB
    XhciEvaluateContextCommandTrb_t evaluateContextTrb;
    zeromem(&evaluateContextTrb, sizeof(XhciEvaluateContextCommandTrb_t));
    evaluateContextTrb.trbType = XHCI_TRB_TYPE_EVALUATE_CONTEXT_CMD;
    evaluateContextTrb.inputContextPhysicalBase = device->getInputContextPhysicalBase();
    evaluateContextTrb.slotId = device->slotId;

    // Send the Evaluate Context command
    XhciCommandCompletionTrb_t* completionTrb = _sendCommand((XhciTrb_t*)&evaluateContextTrb, 200);
    if (!completionTrb) {
        kprintError("[*] Failed to send Evaluate Context command\n");
        return false;
    }

    // Check the completion code
    if (completionTrb->completionCode != XHCI_TRB_COMPLETION_CODE_SUCCESS) {
        kprintError("[*] Evaluate Context command failed with completion code: %s\n",
                    trbCompletionCodeToString(completionTrb->completionCode));
        return false;
    }

    // Optionally, perform a sanity check similar to _addressDevice
    if (m_64ByteContextSize) {
        XhciDeviceContext64* deviceContext = virtbase(m_dcbaa[device->slotId], XhciDeviceContext64);

        kprint("    DeviceContext[slotId=%i] address:0x%llx slotState:%s epState:%s maxPacketSize:%i\n",
            device->slotId, deviceContext->slotContext.deviceAddress,
            xhciSlotStateToString(deviceContext->slotContext.slotState),
            xhciEndpointStateToString(deviceContext->controlEndpointContext.endpointState),
            deviceContext->controlEndpointContext.maxPacketSize
        );
    } else {
        XhciDeviceContext32* deviceContext = virtbase(m_dcbaa[device->slotId], XhciDeviceContext32);

        kprint("    DeviceContext[slotId=%i] address:0x%llx slotState:%s epState:%s maxPacketSize:%i\n",
            device->slotId, deviceContext->slotContext.deviceAddress,
            xhciSlotStateToString(deviceContext->slotContext.slotState),
            xhciEndpointStateToString(deviceContext->controlEndpointContext.endpointState),
            deviceContext->controlEndpointContext.maxPacketSize
        );
    }

    return true;
}

bool XhciDriver::_sendUsbRequestPacket(XhciDevice* device, XhciDeviceRequestPacket& req, void* outputBuffer, uint32_t length) {
    XhciTransferRing* transferRing = device->getControlEndpointTransferRing();

    uint32_t* transferStatusBuffer = (uint32_t*)allocXhciMemory(sizeof(uint32_t), 16, 16);
    uint8_t* descriptorBuffer = (uint8_t*)allocXhciMemory(256, 64, 64);
    
    XhciSetupStageTrb_t setupStage;
    zeromem(&setupStage, sizeof(XhciTrb_t));
    setupStage.trbType = XHCI_TRB_TYPE_SETUP_STAGE;
    setupStage.requestPacket = req;
    setupStage.trbTransferLength = 8;
    setupStage.interrupterTarget = 0;
    setupStage.trt = 3;
    setupStage.idt = 1;
    setupStage.ioc = 0;

    XhciDataStageTrb_t dataStage;
    zeromem(&dataStage, sizeof(XhciTrb_t));
    dataStage.trbType = XHCI_TRB_TYPE_DATA_STAGE;
    dataStage.dataBuffer = physbase(descriptorBuffer);
    dataStage.trbTransferLength = length;
    dataStage.tdSize = 0;
    dataStage.interrupterTarget = 0;
    dataStage.dir = 1;
    dataStage.chain = 1;
    dataStage.ioc = 0;
    dataStage.idt = 0;

    // Clear the status buffer
    *transferStatusBuffer = 0;

    XhciEventDataTrb_t eventDataFirst;
    zeromem(&eventDataFirst, sizeof(XhciTrb_t));
    eventDataFirst.trbType = XHCI_TRB_TYPE_EVENT_DATA;
    eventDataFirst.data = physbase(transferStatusBuffer);
    eventDataFirst.interrupterTarget = 0;
    eventDataFirst.chain = 0;
    eventDataFirst.ioc = 1;

    transferRing->enqueue((XhciTrb_t*)&setupStage);
    transferRing->enqueue((XhciTrb_t*)&dataStage);
    transferRing->enqueue((XhciTrb_t*)&eventDataFirst);
    
    // QEMU doesn't quite handle SETUP/DATA/STATUS transactions correctly.
    // It will wait for the STATUS TRB before it completes the transfer.
    // Technically, you need to check for a good transfer before you send the
    //  STATUS TRB.  However, since QEMU doesn't update the status until after
    //  the STATUS TRB, waiting here will not complete a successful transfer.
    //  Bochs and real hardware handles this correctly, however QEMU does not.
    // If you are using QEMU, do not ring the doorbell here.  Ring the doorbell
    //  *after* you place the STATUS TRB on the ring.
    // (See bug report: https://bugs.launchpad.net/qemu/+bug/1859378 )
    if (!cpuid_isRunningUnderQEMU()) {
        auto completionTrb = _startControlEndpointTransfer(transferRing);
        if (!completionTrb) {
            kfreeAligned(transferStatusBuffer);
            kfreeAligned(descriptorBuffer);
            return false;
        }

        // kprintInfo(
        //     "Transfer Status: %s  Length: %i\n",
        //     trbCompletionCodeToString(completionTrb->completionCode),
        //     completionTrb->transferLength
        // );
    }

    XhciStatusStageTrb_t statusStage;
    zeromem(&statusStage, sizeof(XhciTrb_t));
    statusStage.trbType = XHCI_TRB_TYPE_STATUS_STAGE;
    statusStage.interrupterTarget = 0;
    statusStage.chain = 1;
    statusStage.ioc = 0;
    statusStage.dir = 0;

    // Clear the status buffer
    *transferStatusBuffer = 0;

    XhciEventDataTrb_t eventDataSecond;
    zeromem(&eventDataSecond, sizeof(XhciTrb_t));
    eventDataSecond.trbType = XHCI_TRB_TYPE_EVENT_DATA;
    eventDataSecond.ioc = 1;

    transferRing->enqueue((XhciTrb_t*)&statusStage);
    transferRing->enqueue((XhciTrb_t*)&eventDataSecond);

    auto completionTrb = _startControlEndpointTransfer(transferRing);
    if (!completionTrb) {
        kfreeAligned(transferStatusBuffer);
        kfreeAligned(descriptorBuffer);
        return false;
    }

    // kprintInfo(
    //     "Transfer Status: %s  Length: %i\n",
    //     trbCompletionCodeToString(completionTrb->completionCode),
    //     completionTrb->transferLength
    // );

    // Copy the descriptor into the requested user buffer location
    memcpy(outputBuffer, descriptorBuffer, length);

    kfreeAligned(transferStatusBuffer);
    kfreeAligned(descriptorBuffer);

    return true;
}

bool XhciDriver::_getDeviceDescriptor(XhciDevice* device, UsbDeviceDescriptor* desc, uint32_t length) {
    XhciDeviceRequestPacket req;
    req.bRequestType = 0x80; // Device to Host, Standard, Device
    req.bRequest = 6; // GET_DESCRIPTOR
    req.wValue = USB_DESCRIPTOR_REQUEST(USB_DESCRIPTOR_DEVICE, 0);
    req.wIndex = 0;
    req.wLength = length;

    return _sendUsbRequestPacket(device, req, desc, length);
}

bool XhciDriver::_getStringLanguageDescriptor(XhciDevice* device, UsbStringLanguageDescriptor* desc) {
    XhciDeviceRequestPacket req;
    req.bRequestType = 0x80;
    req.bRequest = 6; // GET_DESCRIPTOR
    req.wValue = USB_DESCRIPTOR_REQUEST(USB_DESCRIPTOR_STRING, 0);
    req.wIndex = 0;
    req.wLength = sizeof(UsbDescriptorHeader);

    // First read just the header in order to get the total descriptor size
    if (!_sendUsbRequestPacket(device, req, desc, sizeof(UsbDescriptorHeader))) {
        kprintError("[XHCI] Failed to read device string language descriptor header\n");
        return false;
    }

    // Read the entire descriptor
    req.wLength = desc->header.bLength;

    if (!_sendUsbRequestPacket(device, req, desc, desc->header.bLength)) {
        kprintError("[XHCI] Failed to read device string language descriptor\n");
        return false;
    }

    return true;
}

bool XhciDriver::_getStringDescriptor(XhciDevice* device, uint8_t descriptorIndex, uint8_t langid, UsbStringDescriptor* desc) {
    XhciDeviceRequestPacket req;
    req.bRequestType = 0x80; // Device to Host, Standard, Device
    req.bRequest = 6; // GET_DESCRIPTOR
    req.wValue = USB_DESCRIPTOR_REQUEST(USB_DESCRIPTOR_STRING, descriptorIndex);
    req.wIndex = langid;
    req.wLength = sizeof(UsbDescriptorHeader);

    // First read just the header in order to get the total descriptor size
    if (!_sendUsbRequestPacket(device, req, desc, sizeof(UsbDescriptorHeader))) {
        kprintError("[XHCI] Failed to read device string descriptor header\n");
        return false;
    }

    // Read the entire descriptor
    req.wLength = desc->header.bLength;

    if (!_sendUsbRequestPacket(device, req, desc, desc->header.bLength)) {
        kprintError("[XHCI] Failed to read device string descriptor\n");
        return false;
    }

    return true;
}

bool XhciDriver::_getConfigurationDescriptor(XhciDevice* device, UsbConfigurationDescriptor* desc) {
    XhciDeviceRequestPacket req;
    req.bRequestType = 0x80; // Device to Host, Standard, Device
    req.bRequest = 6; // GET_DESCRIPTOR
    req.wValue = USB_DESCRIPTOR_REQUEST(USB_DESCRIPTOR_CONFIGURATION, 0);
    req.wIndex = 0;
    req.wLength = sizeof(UsbDescriptorHeader);

    // First read just the header in order to get the total descriptor size
    if (!_sendUsbRequestPacket(device, req, desc, sizeof(UsbDescriptorHeader))) {
        kprintError("[XHCI] Failed to read device configuration descriptor header\n");
        return false;
    }

    // Read the entire descriptor
    req.wLength = desc->header.bLength;

    if (!_sendUsbRequestPacket(device, req, desc, desc->header.bLength)) {
        kprintError("[XHCI] Failed to read device configuration descriptor\n");
        return false;
    }

    // Read the additional bytes for the interface descriptors as well
    req.wLength = desc->wTotalLength;

    if (!_sendUsbRequestPacket(device, req, desc, desc->wTotalLength)) {
        kprintError("[XHCI] Failed to read device configuration descriptor with interface descriptors\n");
        return false;
    }

    return true;
}

bool XhciDriver::_setDeviceConfiguration(XhciDevice* device, uint16_t configurationValue) {
    // Prepare the setup packet
    XhciDeviceRequestPacket setupPacket;
    zeromem(&setupPacket, sizeof(XhciDeviceRequestPacket));
    setupPacket.bRequestType = 0x00; // Host to Device, Standard, Device
    setupPacket.bRequest = 9;        // SET_CONFIGURATION
    setupPacket.wValue = configurationValue;
    setupPacket.wIndex = 0;
    setupPacket.wLength = 0;

    // Perform the control transfer
    if (!_sendUsbRequestPacket(device, setupPacket, nullptr, 0)) {
        kprintError("[XHCI] Failed to set device configuration\n");
        return false;
    }

    return true;
}

bool XhciDriver::_setProtocol(XhciDevice* device, uint8_t interface, uint8_t protocol) {
    XhciDeviceRequestPacket setupPacket;
    zeromem(&setupPacket, sizeof(XhciDeviceRequestPacket));
    setupPacket.bRequestType = 0x21; // Host to Device, Class, Interface
    setupPacket.bRequest = 0x0B;     // SET_PROTOCOL
    setupPacket.wValue = protocol;
    setupPacket.wIndex = interface;
    setupPacket.wLength = 0;

    if (!_sendUsbRequestPacket(device, setupPacket, nullptr, 0)) {
        kprintError("[XHCI] Failed to set device protocol\n");
        return false;
    }

    return true;
}

void XhciDriver::_testKeyboardCommunication(XhciDevice* device) {
    auto transferRing = device->getInterruptInEndpointTransferRing();
    const uint8_t endpointId = 3;
    const uint16_t maxPacketSize = 8;
    uint8_t* dataBuffer = (uint8_t*)allocXhciMemory(maxPacketSize, 64, 64);

    // Prepare a Normal TRB
    XhciNormalTrb_t normalTrb;
    zeromem(&normalTrb, sizeof(XhciNormalTrb_t));
    normalTrb.trbType = XHCI_TRB_TYPE_NORMAL;
    normalTrb.dataBufferPhysicalBase = physbase(dataBuffer);
    normalTrb.trbTransferLength = maxPacketSize;
    normalTrb.ioc = 1; // Interrupt on Completion

    transferRing->enqueue((XhciTrb_t*)&normalTrb);

    m_doorbellManager->ringDoorbell(device->slotId, endpointId);
}
