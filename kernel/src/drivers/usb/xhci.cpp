#include "xhci.h"
#include <paging/page.h>
#include <paging/phys_addr_translation.h>
#include <memory/kmemory.h>
#include <time/ktime.h>
#include <arch/x86/ioapic.h>
#include <interrupts/interrupts.h>
#include <kprint.h>

namespace drivers {
    XhciDriver g_globalXhciInstance;

    void printXhciCapabilityRegisters(volatile XhciCapabilityRegisters* capRegs) {
        kuPrint("Capability Registers:\n");
        kuPrint("CAPLENGTH: %x\n", (uint32_t)capRegs->caplength);
        kuPrint("HCIVERSION: %llx\n", (uint64_t)capRegs->hciversion);
        kuPrint("HCSPARAMS1: %llx\n", capRegs->hcsparams1);
        kuPrint("HCSPARAMS2: %llx\n", capRegs->hcsparams2);
        kuPrint("HCSPARAMS3: %llx\n", capRegs->hcsparams3);
        kuPrint("HCCPARAMS1: %llx\n", capRegs->hccparams1);
        kuPrint("DBOFF: %llx\n", capRegs->dboff);
        kuPrint("RTSOFF: %llx\n", capRegs->rtsoff);
        kuPrint("HCCPARAMS2: %llx\n", capRegs->hccparams2);
        kuPrint("\n");
    }

    void printXhciOperationalRegisters(volatile XhciOperationalRegisters* opRegs) {
        kuPrint("Operational Registers:\n");
        kuPrint("USBCMD: %llx\n", opRegs->usbcmd);
        kuPrint("USBSTS: %llx\n", opRegs->usbsts);
        kuPrint("PAGESIZE: %llx\n", opRegs->pagesize);
        kuPrint("DNCTRL: %llx\n", opRegs->dnctrl);
        kuPrint("CRCR: %llx\n", opRegs->crcr);
        kuPrint("DCBAAP: %llx\n", opRegs->dcbaap);
        kuPrint("CONFIG: %llx\n", opRegs->config);
        kuPrint("\n");
    }

    void xhciInterruptHandler() {
        kuPrint("xhciInterruptHandler fired!\n");
    }

    XhciDriver& XhciDriver::get() {
        return g_globalXhciInstance;
    }

    void printPortscRegister(const XhciPortscRegister& reg) {
        kuPrint("PORTSC Register: raw=0x%x\n", reg.raw);
        kuPrint("CCS: %i\n", reg.bits.ccs);
        kuPrint("PED: %i ", reg.bits.ped);
        kuPrint("TM: %i ", reg.bits.tm);
        kuPrint("OCA: %i ", reg.bits.oca);
        kuPrint("PR: %i\n", reg.bits.pr);
        kuPrint("PLS: %i\n", reg.bits.pls);
        kuPrint("PP: %i\n", reg.bits.pp);
        kuPrint("Port Speed: %i\n", reg.bits.portSpeed);
        kuPrint("PIC: %i ", reg.bits.pic);
        kuPrint("LWS: %i ", reg.bits.lws);
        kuPrint("CSC: %i ", reg.bits.csc);
        kuPrint("PEC: %i\n", reg.bits.pec);
        kuPrint("WRC: %i ", reg.bits.wrc);
        kuPrint("OCC: %i ", reg.bits.occ);
        kuPrint("PRC: %i ", reg.bits.prc);
        kuPrint("PLC: %i ", reg.bits.plc);
        kuPrint("CEC: %i\n", reg.bits.cec);
        kuPrint("CAS: %i ", reg.bits.cas);
        kuPrint("WCE: %i ", reg.bits.wce);
        kuPrint("WDE: %i ", reg.bits.wde);
        kuPrint("WOE: %i\n", reg.bits.woe);
        kuPrint("DR: %i ", reg.bits.dr);
        kuPrint("WPR: %i\n", reg.bits.wpr);
    }

    bool XhciDriver::init(PciDeviceInfo& deviceInfo) {
        _mapDeviceMmio(deviceInfo.barAddress);

        uint64_t opRegBase = (uint64_t)m_capRegisters + m_capRegisters->caplength;
        m_opRegisters = (volatile XhciOperationalRegisters*)opRegBase;

        m_runtimeRegisterBase = opRegBase + m_capRegisters->rtsoff;
        m_rtRegisters = (volatile XhciRuntimeRegisters*)m_runtimeRegisterBase;

        m_maxDeviceSlots = XHCI_MAX_DEVICE_SLOTS(m_capRegisters);
        m_numPorts = XHCI_NUM_PORTS(m_capRegisters);

        // printXhciCapabilityRegisters(m_capRegisters);
        // printXhciOperationalRegisters(m_opRegisters);

        if (!_resetController()) {
            return false;
        }
        kuPrint("[XHCI] Reset the controller\n");

        // Set the Max Device Slots Enabled field
        uint32_t configReg = XHCI_SET_MAX_SLOTS_EN(m_opRegisters->config, m_maxDeviceSlots);
        m_opRegisters->config = configReg;

        // Initialize device context base address array
        if (!_initializeDcbaa()) {
            return false;
        }
        kuPrint("[XHCI] Initialized device context array\n");

        kuPrint("System has %i ports and %i device slots\n", m_numPorts, m_maxDeviceSlots);
        
        /*
        kuPrint("IRQ Line: %i\n", deviceInfo.headerInfo.interruptLine);

        kuPrint("Rerouting interrupt line: %i -> %i\n", deviceInfo.headerInfo.interruptLine, IRQ1);
        mapIoApicIrq(deviceInfo.headerInfo.interruptLine, IRQ1, 0);

        // if (HAS_PCI_CAP(deviceInfo, PciCapabilityMsiX)) {
        //     uint32_t msixCapOffset = 0;
        //     PciMsiXCapability msixCap = _readMsixCapability(deviceInfo.bus, deviceInfo.device, deviceInfo.function, msixCapOffset);
        //     uint16_t numEntries = (msixCap.messageControl & 0x7FF) + 1;
        //     kuPrint("MSI-X Table Entries: %i\n", numEntries);
        //     kuPrint("MSI-X Table Offset: 0x%llx\n", msixCap.tableOffset & ~0x7);

        //     MsiXTableEntry* msixTable = (MsiXTableEntry*)kmallocAligned(sizeof(MsiXTableEntry) * numEntries, 64);

        //     for (int i = 0; i < numEntries; ++i) {
        //         msixTable[i].messageAddress = getLocalApicPhysicalBase();
        //         msixTable[i].messageData = IRQ1 + i;
        //         msixTable[i].vectorControl = 0; // Unmask the interrupt
        //     }

        //     uint32_t msixTableOffset = msixCap.tableOffset & ~0x7;
        //     uint64_t msixTablePhysAddr = m_xhcBase + msixTableOffset;
        //     kuPrint("msixTablePhysAddr: 0x%llx\n", msixTablePhysAddr);

        //     paging::mapPage((void*)msixTablePhysAddr, (void*)msixTablePhysAddr, USERSPACE_PAGE, paging::g_kernelRootPageTable);

        //     memcpy((void*)msixTablePhysAddr, msixTable, sizeof(MsiXTableEntry) * numEntries);

        //     uint16_t msixControl = pciConfigRead16(deviceInfo.bus, deviceInfo.device, deviceInfo.function, msixCapOffset);
        //     msixControl |= 0x8000; // Set the MSI-X Enable bit
        //     pciConfigWrite16(deviceInfo.bus, deviceInfo.device, deviceInfo.function, msixCapOffset, msixControl);
        // } else if (HAS_PCI_CAP(deviceInfo, PciCapabilityMsi)) {
        //     uint32_t msiCapOffset = 0;
        //     // Read the MSI capability structure
        //     PciMsiCapability msiCap = _readMsiCapability(deviceInfo.bus, deviceInfo.device, deviceInfo.function, msiCapOffset);

        //     // Configure the MSI message address and data
        //     uint32_t msiAddress = getLocalApicPhysicalBase(); // Address of the local APIC
        //     uint16_t msiData = IRQ1; // Data, typically an interrupt vector

        //     // Write the address and data to the appropriate MSI registers
        //     pciConfigWrite32(deviceInfo.bus, deviceInfo.device, deviceInfo.function, msiCapOffset + offsetof(PciMsiCapability, messageAddress), msiAddress);
        //     pciConfigWrite16(deviceInfo.bus, deviceInfo.device, deviceInfo.function, msiCapOffset + offsetof(PciMsiCapability, messageData), msiData);

        //     // Enable MSI in the control register
        //     uint16_t msiControl = pciConfigRead16(deviceInfo.bus, deviceInfo.device, deviceInfo.function, msiCapOffset + offsetof(PciMsiCapability, messageControl));
        //     msiControl |= 0x0001; // Set the MSI Enable bit
        //     pciConfigWrite16(deviceInfo.bus, deviceInfo.device, deviceInfo.function, msiCapOffset + offsetof(PciMsiCapability, messageControl), msiControl);
        // }

        
        */
       
        if (!_setupEventRing()) {
            kuPrint("[XHCI] Failed to setup Event Ring\n");
            return false;
        }

        if (!_setupCommandRing()) {
            kuPrint("[XHCI] Failed to setup Command Ring\n");
            return false;
        }

        uint32_t dbOffset = _readDoorbellArrayOffset();
        m_doorbellArrayBase = m_xhcBase + (uint64_t)dbOffset;
       
        // Enable interrupts
        // _enableInterrupter(0);
        // kuPrint("Interrupts enabled for interrupter 0\n");

        kuPrint("\n");

        // Enable the controller
        _enableController();

        printXhciOperationalRegisters(m_opRegisters);

        // Create an Enable Slot Command TRB
        XhciTransferRequestBlock trb;
        // Zero out the TRB to start with a clean state
        zeromem(&trb, sizeof(XhciTransferRequestBlock));

        // Set the TRB type to 'Enable Slot Command'
        trb.control = (XHCI_TRB_TYPE_NOOP_CMD << XHCI_TRB_TYPE_SHIFT);

        // // Copy the TRB to the current position in the Command Ring
        // m_commandRing[0] = trb;
        // m_commandRing[0].control |= XHCI_TRB_CYCLE_BIT;

        // kuPrint("Added TRB to command ring, usbsts: %llx\n", m_opRegisters->usbsts);

        // kuPrint("Dumping the first 2 TRBs from the command ring:\n");

        // for (int i = 0; i < 2; ++i) {
        //     XhciTrb_t* currentTrb = &m_commandRing[i];

        //     kuPrint("TRB %i:\n", i);
        //     kuPrint("  Parameter: 0x%llx\n", currentTrb->parameter);
        //     kuPrint("  Status:    0x%llx\n", currentTrb->status);
        //     kuPrint("  Control:   0x%llx\n\n", currentTrb->control);
        // }

        // // Ring the command ring doorbell
        // _ringCommandRingDoorbell();

        // sleep(1);
        // kuPrint("Rung command ring doorbell, usbsts: %llx\n", m_opRegisters->usbsts);

        // if (_checkForHostControllerError()) {
        //     kuPrint("[XHCI] HOST CONTROLLER ERROR!\n");
        //     printXhciOperationalRegisters(m_opRegisters);
        //     while (1);
        // }

        // // Wait for command completion event
        // //_waitForCommandCompletionEvent();

        // kuPrint("Dumping the first 5 TRBs from the event ring:\n");

        // for (int i = 0; i < 2; ++i) {
        //     XhciTrb_t* currentTrb = &m_eventRing[i];

        //     kuPrint("TRB %i:\n", i);
        //     kuPrint("  Parameter: 0x%llx\n", currentTrb->parameter);
        //     kuPrint("  Status:    0x%llx\n", currentTrb->status);
        //     kuPrint("  Control:   0x%llx\n\n", currentTrb->control);
        // }

        //_allocateSlot();

        for (uint32_t i = 1; i <= m_numPorts; i++) {
            XhciPortscRegister portscReg;
            _readPortscReg(i, portscReg);
            
            if (portscReg.bits.ccs) {
                _resetPort(i);

                kuPrint("--- Port %i: Connected ----\n", i);
                switch (portscReg.bits.portSpeed) {
                case USB_SPEED_FULL: kuPrint("   speed: 12 Mb/s (Full Speed)\n"); break;
                case USB_SPEED_LOW: kuPrint("   speed: 1.5 Mb/s (Low Speed)\n"); break;
                case USB_SPEED_HIGH: kuPrint("   speed: 480 Mb/s (High Speed)\n"); break;
                case USB_SPEED_SUPER: kuPrint("   speed: 5000 Mb/s (SuperSpeed)\n"); break;
                case USB_SPEED_SUPER_PLUS: kuPrint("   speed: 10000 Mb/s (SuperSpeedPlus)\n"); break;
                default:break;
                }
                kuPrint("\n");
            }
        }

        return true;
    }

    void XhciDriver::_mapDeviceMmio(uint64_t pciBarAddress) {
        // Map a conservatively large space for xHCI registers
        for (size_t offset = 0; offset < 0x20000; offset += PAGE_SIZE) {
            paging::mapPage((void*)(pciBarAddress + offset), (void*)(pciBarAddress + offset), USERSPACE_PAGE, paging::g_kernelRootPageTable);
        }

        m_xhcBase = pciBarAddress;
        m_capRegisters = (volatile XhciCapabilityRegisters*)pciBarAddress;
    }

    void XhciDriver::_writeUsbRegCommand(uint32_t cmd) {
        m_opRegisters->usbcmd |= cmd;
    }
    
    bool XhciDriver::_readUsbRegStatusFlag(uint32_t flag) {
        return (m_opRegisters->usbsts & flag);
    }

    bool XhciDriver::_isControllerReady() {
        return !_readUsbRegStatusFlag(XHCI_USBSTS_CNR);
    }

    bool XhciDriver::_is64ByteContextUsed() {
        return (XHCI_CSZ(m_capRegisters));
    }

    bool XhciDriver::_resetController() {
        _writeUsbRegCommand(XHCI_USBCMD_HCRESET);

        // Make sure the controller's CNR flag is cleared
        while (!_isControllerReady()) {
            msleep(16);
        }

        // Wait for the usbcmd reset flag to get cleared
        while (m_opRegisters->usbcmd & XHCI_USBCMD_HCRESET) {
            msleep(16);
        }

        // Check the Host System Error flag for error control
        return !(m_opRegisters->usbsts & XHCI_USBSTS_HSE);
    }

    void XhciDriver::_enableController() {
        m_opRegisters->usbcmd |= XHCI_USBCMD_RUN_STOP;

        // Make sure the controller's CNR flag is cleared
        while (m_opRegisters->usbsts & XHCI_USBSTS_HCH) {
            msleep(16);
        }
    }

    bool XhciDriver::_checkForHostControllerError() {
        return m_opRegisters->usbsts & XHCI_USBSTS_HCE;
    }

    bool XhciDriver::_initializeDcbaa() {
        // The address has to be 64-byte aligned
        uint64_t* dcbaapVirtual = (uint64_t*)kmallocAligned(sizeof(uint64_t) * m_maxDeviceSlots, 64);
        if (!dcbaapVirtual) {
            return false;
        }

        // Make sure each entry is zeroed out
        zeromem(dcbaapVirtual, sizeof(uint64_t) * m_maxDeviceSlots);

        // Initialize the device context array
        _initializeDeviceContexts((XhciDeviceContext**)dcbaapVirtual);

        // Get the physical address
        void* dcbaapPhysical = __pa(dcbaapVirtual);

        // Update operational register with the physical address of DCBAA
        m_opRegisters->dcbaap = reinterpret_cast<uint64_t>(dcbaapPhysical);

        return true;
    }

    bool XhciDriver::_initializeDeviceContexts(XhciDeviceContext** dcbaap) {
        for (uint32_t slot = 1; slot <= m_maxDeviceSlots; ++slot) {
            XhciDeviceContext* deviceContext = (XhciDeviceContext*)kmallocAligned(sizeof(XhciDeviceContext), 64);
            break;
            if (!deviceContext) {
                return false;
            }
            zeromem(deviceContext, sizeof(XhciDeviceContext));

            // Configure the Control Endpoint Context
            _configureControlEndpoint(&deviceContext->endpointContext[0]);

            // Store the Device Context in the DCBAA
            dcbaap[slot] = deviceContext;
        }

        /*
        // xHci Spec Section 6.1 (page 404)

        If the Max Scratchpad Buffers field of the HCSPARAMS2 register is > ‘0’, then
        the first entry (entry_0) in the DCBAA shall contain a pointer to the Scratchpad
        Buffer Array. If the Max Scratchpad Buffers field of the HCSPARAMS2 register is
        = ‘0’, then the first entry (entry_0) in the DCBAA is reserved and shall be
        cleared to ‘0’ by software.
        */
        uint32_t maxScratchpadBuffers = XHCI_MAX_SCRATCHPAD_BUFFERS(m_capRegisters);

        // TO-DO: if maxScratchpadBuffers > 0, initialize scratchpad buffer array
        if (maxScratchpadBuffers > 0) {
            kuPrint("TO-DO: if maxScratchpadBuffers > 0, initialize scratchpad buffer array\n");
        }

        return true;
    }

    void XhciDriver::_configureControlEndpoint(XhciEndpointContext* ctx) {
        // Zero out the endpoint context
        zeromem(ctx, sizeof(XhciEndpointContext));

        // Set the Max Packet Size for the control endpoint (usually 64 for USB 2.0, 512 for USB 3.0)
        // This may vary based on the USB version and device capabilities.
        ctx->maxPacketSize = 512;

        // Set the Interval for the control endpoint
        // For control endpoints, this is usually set to 0.
        ctx->interval = 0;
    }

    void XhciDriver::_readPortscReg(uint32_t portNum, XhciPortscRegister& reg) {
        // Operational Base + (400h + (10h * (n – 1)))
        uint64_t opbase = (uint64_t)m_opRegisters;
        uint64_t portscBase = opbase + (0x400 + (0x10 * (portNum - 1)));

        volatile uint32_t* hwreg = (volatile uint32_t*)portscBase;

        reg.raw = *hwreg;
    }

    void XhciDriver::_writePortscReg(uint32_t portNum, XhciPortscRegister& reg) {
        // Operational Base + (400h + (10h * (n – 1)))
        uint64_t opbase = (uint64_t)m_opRegisters;
        uint64_t portscBase = opbase + (0x400 + (0x10 * (portNum - 1)));

        volatile uint32_t* hwreg = (volatile uint32_t*)portscBase;

        *hwreg = reg.raw;
    }

    void XhciDriver::_resetPort(uint32_t portNum) {
        // // Check the overall controller status
        // uint32_t usbsts = m_opRegisters->usbsts;
        // kuPrint("USBSTS before port reset: 0x%x\n", usbsts);

        // // Check the status of the port
        // XhciPortscRegister reg;
        // zeromem(&reg, sizeof(XhciPortscRegister));
        // _readPortscReg(portNum, reg);
        // kuPrint("PORTSC Register (raw value): 0x%x\n", reg.raw);
        // kuPrint("CCS (Current Connect Status): %llu\n", reg.bits.ccs);
        // kuPrint("PED (Port Enabled/Disabled): %llu\n", reg.bits.ped);
        // kuPrint("TM (Test Mode): %llu\n", reg.bits.tm);
        // kuPrint("OCA (Overcurrent Active): %llu\n", reg.bits.oca);
        // kuPrint("PR (Port Reset): %llu\n", reg.bits.pr);
        // kuPrint("PLS (Port Link State): %llu\n", reg.bits.pls);
        // kuPrint("PP (Port Power): %llu\n", reg.bits.pp);
        // kuPrint("Port Speed: %llu\n", reg.bits.portSpeed);
        // kuPrint("PIC (Port Indicator Control): %llu\n", reg.bits.pic);
        // kuPrint("LWS (Link Write Strobe): %llu\n", reg.bits.lws);
        // kuPrint("CSC (Connect Status Change): %llu\n", reg.bits.csc);
        // kuPrint("PEC (Port Enabled/Disabled Change): %llu\n", reg.bits.pec);
        // kuPrint("WRC (Warm Reset Change): %llu\n", reg.bits.wrc);
        // kuPrint("OCC (Overcurrent Change): %llu\n", reg.bits.occ);
        // kuPrint("PRC (Port Reset Change): %llu\n", reg.bits.prc);
        // kuPrint("PLC (Port Link State Change): %llu\n", reg.bits.plc);
        // kuPrint("CEC (Config Error Change): %llu\n", reg.bits.cec);
        // kuPrint("CAS (Cold Attach Status): %llu\n", reg.bits.cas);
        // kuPrint("WCE (Wake on Connect Enable): %llu\n", reg.bits.wce);
        // kuPrint("WDE (Wake on Disconnect Enable): %llu\n", reg.bits.wde);
        // kuPrint("WOE (Wake on Overcurrent Enable): %llu\n", reg.bits.woe);
        // kuPrint("DR (Data Role): %llu\n", reg.bits.dr);
        // kuPrint("WPR (Warm Port Reset): %llu\n", reg.bits.wpr);

        XhciPortscRegister portscReg;

        _readPortscReg(portNum, portscReg);
        portscReg.bits.pr = 1;
        _writePortscReg(portNum, portscReg);

        do {
            _readPortscReg(portNum, portscReg);
            msleep(10);
        } while (portscReg.bits.pr);

        if (portscReg.bits.ped) {
            kuPrint("Port %llu: Enabled and Reset complete\n", portNum);
        } else {
            kuPrint("Port %llu: Reset not completed or port disabled\n", portNum);
        }

        _readPortscReg(portNum, portscReg);
        portscReg.bits.prc = 1;
        _writePortscReg(portNum, portscReg);
    }

    void XhciDriver::_resetPorts() {
        for (uint32_t i = 1; i <= m_numPorts; i++) {
            _resetPort(i);
        }
    }

    void XhciDriver::_readImanReg(uint32_t interrupter, XhciInterrupterManagementRegister& reg) {
        // Address: Runtime Base + 020h + (32 * Interrupter)
        //          where: Interrupter is 0, 1, 2, 3, … 1023
        uint64_t base = m_runtimeRegisterBase + 0x20 + (32 * interrupter);
        volatile uint32_t* hwreg = (volatile uint32_t*)base;

        reg.raw = *hwreg;
    }

    void XhciDriver::_writeImanReg(uint32_t interrupter, XhciInterrupterManagementRegister& reg) {
        // Address: Runtime Base + 020h + (32 * Interrupter)
        //          where: Interrupter is 0, 1, 2, 3, … 1023
        uint64_t base = m_runtimeRegisterBase + 0x20 + (32 * interrupter);
        volatile uint32_t* hwreg = (volatile uint32_t*)base;

        *hwreg = reg.raw;
    }

    void XhciDriver::_enableInterrupter(uint32_t interrupter) {
        XhciInterrupterManagementRegister imanReg;
        
        _readImanReg(interrupter, imanReg);
        imanReg.bits.interruptEnabled = 1;
        _writeImanReg(interrupter, imanReg);
    }

    void XhciDriver::_acknowledgeInterrupt(uint32_t interrupter) {
        XhciInterrupterManagementRegister imanReg;
        
        _readImanReg(interrupter, imanReg);
        imanReg.bits.interruptPending = 1;
        _writeImanReg(interrupter, imanReg);
    }

    bool XhciDriver::_setupEventRing() {
        m_eventRingSize = 1024;
        m_eventRingDequeueIndex = 0;
        m_eventRingCycleState = true;

        // Allocate memory for the ERST
        XhciErstEntry* eventRingSegmentTable = (XhciErstEntry*)kmallocAligned(sizeof(XhciErstEntry) * 1, 64);
        if (!eventRingSegmentTable) {
            kuPrint("Failed to allocate ERST\n");
            return false;
        }

        kuPrint("ERST Allocated at: %llx\n", (uint64_t)eventRingSegmentTable);

        // Allocate memory for the Event Ring segment
        m_eventRing = (XhciTransferRequestBlock*)kmallocAligned(sizeof(XhciTransferRequestBlock) * m_eventRingSize, 64);
        if (!m_eventRing) {
            kuPrint("Failed to allocate Event Ring Segment\n");
            return false;
        }

        // Make sure the ring is zero'ed out
        zeromem(m_eventRing, sizeof(XhciTransferRequestBlock) * m_eventRingSize);

        kuPrint("Event Ring Segment Allocated at: %llx\n", (uint64_t)m_eventRing);

        // Ensure that m_defaultEventRingSize is correctly initialized
        if (m_eventRingSize == 0) {
            kuPrint("defaultEventRingSize is zero\n");
            return false;
        }

        // Initialize ERST entry to point to the Event Ring segment
        eventRingSegmentTable[0].ringSegmentBaseAddress = (uint64_t)__pa(m_eventRing);
        eventRingSegmentTable[0].ringSegmentSize = m_eventRingSize;
        eventRingSegmentTable[0].rsvd = 0;

        kuPrint("ERST Entry Base Address: %llx\n", eventRingSegmentTable[0].ringSegmentBaseAddress);
        kuPrint("ERST Entry Segment Size: %llx\n", eventRingSegmentTable[0].ringSegmentSize);

        // Write to ERSTBA register
        volatile uint64_t* erstba = _getErstbaRegAddress(0);
        *erstba = (uint64_t)__pa(eventRingSegmentTable);

        // Write to ERSTSZ register
        volatile uint32_t* erstsz = _getErstszRegAddress(0);
        *erstsz = 1;

        // Initialize and set ERDP
        volatile uint64_t* erdp = _getErdpRegAddress(0);
        *erdp = (uint64_t)__pa(m_eventRing);

        kuPrint("ERDP: %llx\n", *erdp);

        return true;
    }

    volatile uint32_t* XhciDriver::_getErstszRegAddress(uint32_t interrupter) {
        // Address: Runtime Base + 028h + (32 * Interrupter)
        //          where: Interrupter is 0, 1, 2, 3, … 1023
        uint64_t base = m_runtimeRegisterBase + 0x28 + (32 * interrupter);

        return (volatile uint32_t*)base;
    }

    volatile uint64_t* XhciDriver::_getErstbaRegAddress(uint32_t interrupter) {
        // Address: Runtime Base + 030h + (32 * Interrupter)
        //          where: Interrupter is 0, 1, 2, 3, … 1023
        uint64_t base = m_runtimeRegisterBase + 0x30 + (32 * interrupter);

        return (volatile uint64_t*)base;
    }

    volatile uint64_t* XhciDriver::_getErdpRegAddress(uint32_t interrupter) {
        // Address: Runtime Base + 038h + (32 * Interrupter)
        //          where: Interrupter is 0, 1, 2, 3, … 1023
        uint64_t base = m_runtimeRegisterBase + 0x38 + (32 * interrupter);

        return (volatile uint64_t*)base;
    }

    void XhciDriver::_waitForCommandCompletionEvent() {
        kuPrint("Waiting for command completion event...\n");

        while (true) {
            kuPrint("CRR Running? %lli\n", m_opRegisters->crcr & XHCI_CRCR_COMMAND_RING_RUNNING);
            XhciTransferRequestBlock& eventTrb = m_eventRing[m_eventRingDequeueIndex];

            // Log the current state of the Event Ring dequeue index and the TRB's control field
            kuPrint("Checking Event TRB at index %i, Control: 0x%x, Cycle bit: %i\n", m_eventRingDequeueIndex, eventTrb.control, eventTrb.control & XHCI_TRB_CYCLE_BIT);

            if ((eventTrb.control & XHCI_TRB_CYCLE_BIT) == m_eventRingCycleState) {
                kuPrint("Found a new TRB to process.\n");

                if ((eventTrb.control & XHCI_TRB_TYPE_MASK) == XHCI_TRB_TYPE_CMD_COMPLETION_EVENT) {
                    kuPrint("Found Command Completion Event TRB.\n");

                    uint32_t slotID = (eventTrb.status & XHCI_SLOT_ID_MASK) >> XHCI_SLOT_ID_SHIFT;
                    uint32_t completionCode = (eventTrb.status & XHCI_COMPLETION_CODE_MASK) >> XHCI_COMPLETION_CODE_SHIFT;

                    kuPrint("Completion Code: %i, Slot ID: %i\n", completionCode, slotID);

                    if (completionCode != XHCI_COMPLETION_CODE_SUCCESS) {
                        kuPrint("Command failed with completion code: %i\n", completionCode);
                        return; // Or handle error appropriately
                    }

                    kuPrint("Command completed successfully, allocated Slot ID: %i\n", slotID);
                    return;
                }

                if (++m_eventRingDequeueIndex >= m_eventRingSize) {
                    m_eventRingDequeueIndex = 0;
                    m_eventRingCycleState = !m_eventRingCycleState;
                    kuPrint("Event Ring wrapped around. New Cycle State: %i\n", m_eventRingCycleState);
                }
            } else {
                kuPrint("No new TRB to process yet, waiting...\n");
                msleep(1000); // Sleep for a while before checking again
            }
        }
    }


    uint32_t XhciDriver::_readDoorbellArrayOffset() {
        // Address: Base + (14h)
        uint64_t addr = m_xhcBase + 0x14;
        volatile uint32_t* hwreg = (volatile uint32_t*)addr;

        uint32_t dboff = *hwreg;
        return dboff;
    }

    void XhciDriver::_readDoorbellRegister(uint32_t doorbell, XhciDoorbellRegister& reg) {
        volatile uint32_t* hwreg = reinterpret_cast<volatile uint32_t*>(
            reinterpret_cast<char*>(m_doorbellArrayBase) + (doorbell * sizeof(uint32_t))
        );

        // 32-bit dword read
        reg.raw = *hwreg;
    }
    
    void XhciDriver::_writeDoorbellRegister(uint32_t doorbell, XhciDoorbellRegister& reg) {
        volatile uint32_t* hwreg = reinterpret_cast<volatile uint32_t*>(
            reinterpret_cast<char*>(m_doorbellArrayBase) + (doorbell * sizeof(uint32_t))
        );

        // 32-bit dword write
        *hwreg = reg.raw;
    }

    void XhciDriver::_ringCommandRingDoorbell() {
        XhciDoorbellRegister doorbellReg;
        doorbellReg.raw = 0; // Clear the register
        doorbellReg.dbTarget = XHCI_DOORBELL_TARGET_COMMAND_RING;

        kuPrint("Ringing Command Ring Doorbell, target: %i\n", doorbellReg.dbTarget);

        _writeDoorbellRegister(0, doorbellReg);

        // Read back the doorbell register to verify the write operation
        XhciDoorbellRegister verifyReg;
        _readDoorbellRegister(0, verifyReg);
        kuPrint("Doorbell Register after ringing: 0x%x\n", verifyReg.raw);
    }

    bool XhciDriver::_setupCommandRing() {
        m_commandRingSize = 256;
        m_commandRingEnqueueIndex = 0;
        m_commandRingCycleState = true;

        // Allocate memory for the Command Ring
        m_commandRing = (XhciTransferRequestBlock*)kmallocAligned(sizeof(XhciTransferRequestBlock) * m_commandRingSize, 64);
        if (!m_commandRing) {
            return false;
        }

        // Zero out and initialize the Command Ring
        for (uint64_t i = 0; i < m_commandRingSize; ++i) {
            // Initialize each TRB, setting the Cycle Bit appropriately
            zeromem(&m_commandRing[i], sizeof(XhciTransferRequestBlock));

            /*
                The Cycle bit shall be initialized by software to ‘0’ in all TRBs of all
                segments when initializing a ring (page 157).
            */
        }

        // Set the Link TRB at the end of the Command Ring
        m_commandRing[m_commandRingSize - 1].parameter = (uint64_t)__pa(m_commandRing); // Physical address of the start of the ring
        m_commandRing[m_commandRingSize - 1].control = XHCI_TRB_TYPE_LINK | XHCI_TRB_TOGGLE_CYCLE;

        // Write the physical base address of the Command Ring to the CRCR register
        uint64_t crcr = (uint64_t)__pa(m_commandRing) | XHCI_CRCR_RING_CYCLE_STATE;

        m_opRegisters->crcr = crcr;

        msleep(10);

        kuPrint("[DBG] Written crcr with  : %llx\n", crcr);
        return true;
    }

    void XhciDriver::_prepareEnableSlotTrb(XhciTransferRequestBlock& trb) {
        // Zero out the TRB to start with a clean state
        zeromem(&trb, sizeof(XhciTransferRequestBlock));

        // Set the TRB type to 'Enable Slot Command'
        trb.control = (XHCI_TRB_TYPE_ENABLE_SLOT_CMD << XHCI_TRB_TYPE_SHIFT);
    }
    
    void XhciDriver::_addTrbToCommandRing(XhciTransferRequestBlock& trb) {
        // Copy the TRB to the current position in the Command Ring
        m_commandRing[m_commandRingEnqueueIndex] = trb;

        // Toggle the Cycle Bit if necessary
        if (m_commandRingCycleState) {
            m_commandRing[m_commandRingEnqueueIndex].control |= XHCI_TRB_CYCLE_BIT;
        } else {
            m_commandRing[m_commandRingEnqueueIndex].control &= ~XHCI_TRB_CYCLE_BIT;
        }

        // Increment the enqueue index and wrap around if at the end of the ring
        if (++m_commandRingEnqueueIndex >= m_commandRingSize) {
            m_commandRingEnqueueIndex = 0;
            // Toggle the cycle state each time we wrap the ring
            m_commandRingCycleState = !m_commandRingCycleState;
        }
    }

    uint32_t XhciDriver::_allocateSlot() {
        // Create an Enable Slot Command TRB
        XhciTransferRequestBlock enableSlotTrb;
        _prepareEnableSlotTrb(enableSlotTrb);

        // Add the TRB to the Command Ring
        _addTrbToCommandRing(enableSlotTrb);

        // Ring the command ring doorbell
        _ringCommandRingDoorbell();

        // Wait for command completion event
        _waitForCommandCompletionEvent();

        return 0;
    }
} // namespace drivers
