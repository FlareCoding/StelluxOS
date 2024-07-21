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

    void printXhciCapabilityRegisters(volatile XhciCapabilityRegisters* capRegs) {
        kuPrint("Capability Registers: (%llx)\n", (uint64_t)capRegs);
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
        kuPrint("Operational Registers: (%llx)\n", (uint64_t)opRegs);
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

        printXhciCapabilityRegisters(m_capRegisters);

        // Enable Bus Mastering
        enableBusMastering(deviceInfo.bus, deviceInfo.device, deviceInfo.function);
        kuPrint("[XHCI] Enabled bus mastering\n");

        // Controller takeover
        _assumeOwnershipFromBios();
        kuPrint("[XHCI] Performed BIOS to OS handoff\n");

        if (!_resetController()) {
            return false;
        }
        kuPrint("[XHCI] Reset the controller\n");

        // Set the Max Device Slots Enabled field
        uint32_t configReg = XHCI_SET_MAX_SLOTS_EN(m_opRegisters->config, m_maxDeviceSlots);
        m_opRegisters->config = configReg;

        m_opRegisters->dnctrl |= 0b1111111111111111;

        if (_checkForHostControllerError()) {
            return false;   
        }

        // ============================== Initialize DCBAA ============================== //
        kuPrint("[XHCI] device slots: %llu   ports: %llu\n", m_maxDeviceSlots, m_numPorts);
        kuPrint("[XHCI] 64-byte contexts: %s\n", _is64ByteContextUsed() ? "true" : "false");

        size_t contextEntrySize = _is64ByteContextUsed() ? 64 : 32;
        void* dcbaaVirtualBase = _allocXhciMemory(contextEntrySize * (m_maxDeviceSlots + 1));
        zeromem(dcbaaVirtualBase, contextEntrySize * (m_maxDeviceSlots + 1));
        kuPrint("[XHCI] dcbaa va: %llx  pa: %llx\n", dcbaaVirtualBase, (uint64_t)__pa(dcbaaVirtualBase));

        /*
        // xHci Spec Section 6.1 (page 404)

        If the Max Scratchpad Buffers field of the HCSPARAMS2 register is > ‘0’, then
        the first entry (entry_0) in the DCBAA shall contain a pointer to the Scratchpad
        Buffer Array. If the Max Scratchpad Buffers field of the HCSPARAMS2 register is
        = ‘0’, then the first entry (entry_0) in the DCBAA is reserved and shall be
        cleared to ‘0’ by software.
        */
        uint32_t maxScratchpadBuffers = XHCI_MAX_SCRATCHPAD_BUFFERS(m_capRegisters);
        kuPrint("[XHCI] Scratchpad buffer: %s\n", maxScratchpadBuffers > 0 ? "required" : "not required");

        // Initialize scratchpad buffer array if needed
        if (maxScratchpadBuffers > 0) {
            void* scratchpadArray = _allocXhciMemory(contextEntrySize * maxScratchpadBuffers);
            zeromem(scratchpadArray, contextEntrySize * maxScratchpadBuffers);

            ((uint64_t*)dcbaaVirtualBase)[0] = (uint64_t)__pa(scratchpadArray);
        }

        m_opRegisters->dcbaap = (uint64_t)__pa(dcbaaVirtualBase);

        //  ============================== Initialize command ring ============================== //
        size_t cmdRingSize = sizeof(XhciTrb_t) * 256;
        XhciTrb_t* commandRing = (XhciTrb_t*)_allocXhciMemory(cmdRingSize);
        zeromem(commandRing, cmdRingSize);

        // Set the last TRB as a link TRB to point back to the first TRB
        commandRing[255].parameter = (uint64_t)__pa(commandRing);
        commandRing[255].control = (XHCI_TRB_TYPE_LINK << 10) | XHCI_CRCR_RING_CYCLE_STATE;

        // Set the CRCR register to point to the command ring
        uint64_t cmdRingPhysicalBase = (uint64_t)__pa(commandRing);
        m_opRegisters->crcr = cmdRingPhysicalBase | XHCI_CRCR_RING_CYCLE_STATE;

        kuPrint("[XHCI] Command ring initialized: %llx\n", (uint64_t)commandRing);

        //  ============================== Initialize event ring  ============================== //
        // Define the size of the event ring
        const uint32_t eventRingTRBs = 256;
        const size_t eventRingSize = eventRingTRBs * sizeof(XhciTrb_t);

        // Allocate memory for the event ring
        void* eventRingVirtualBase = _allocXhciMemory(eventRingSize);
        zeromem(eventRingVirtualBase, eventRingSize);

        XhciTrb_t* eventRing = (XhciTrb_t*)eventRingVirtualBase;

        // Allocate memory for the Event Ring Segment Table (ERST)
        XhciErstEntry* erst = (XhciErstEntry*)_allocXhciMemory(sizeof(XhciErstEntry) * 1);
        zeromem(erst, sizeof(XhciErstEntry) * 1);

        // Initialize the ERST entries
        erst[0].ringSegmentBaseAddress = (uint64_t)__pa(eventRingVirtualBase);
        erst[0].ringSegmentSize = eventRingTRBs; // Number of TRBs in this segment

        // Configure the Event Ring Segment Table Size (ERSTSZ) register
        volatile uint32_t* erstsz = _getErstszRegAddress(0);
        *erstsz = 1;

        // Initialize and set ERDP
        volatile uint64_t* erdp = _getErdpRegAddress(0);
        *erdp = (uint64_t)__pa(eventRingVirtualBase);

        // Write to ERSTBA register
        volatile uint64_t* erstba = _getErstbaRegAddress(0);
        *erstba = (uint64_t)__pa(erst);

        kuPrint("[XHCI] Event ring initialized:\n");
        kuPrint("    Event ring VA: %llx, PA: %llx\n", (uint64_t)eventRing, (uint64_t)__pa(eventRing));
        kuPrint("    ERST entry VA: %llx, PA: %llx\n", (uint64_t)erst, (uint64_t)__pa(erst));
        kuPrint("    ERSTBA: %llx\n", *erstba);
        kuPrint("    ERSTSZ: %x\n", *erstsz);
        kuPrint("    ERDP: %llx\n", *erdp);

        _enableController();

        if (_checkForHostControllerError()) {
            return false;
        }

        kuPrint("[XHCI] Started the controller\n");

        printXhciOperationalRegisters(m_opRegisters);

        //  ============================== Test HC Communication  ============================== //
        // Save the initial value of CRCR
        //uint64_t initialCrcr = m_opRegisters->crcr;

        // Allocate and clear a test TRB
        XhciTrb_t testTrb;
        zeromem(&testTrb, sizeof(XhciTrb_t));
        testTrb.control = (XHCI_TRB_TYPE_ENABLE_SLOT_CMD << 10) | 1;
        commandRing[0] = testTrb;

        XhciTrb_t noOpTrb;
        zeromem(&noOpTrb, sizeof(XhciTrb_t));
        noOpTrb.control = (XHCI_TRB_TYPE_NOOP_CMD << 10) | 1;
        commandRing[1] = noOpTrb;

        // Ring the doorbell for slot 0 (command ring)
        uint64_t doorbellArrayBase = (uint64_t)m_capRegisters + m_capRegisters->dboff;
        volatile uint32_t* db1 = (volatile uint32_t*)doorbellArrayBase;
        *db1 = 0;

        kuPrint("[XHCI] Sent a test TRB\n");

        if (_checkForHostControllerError()) {
            return false;
        }

        // Check the CRCR value
        // kuPrint("[XHCI] Initial CRCR: %llx\n", initialCrcr);
        // kuPrint("[XHCI] Updated CRCR: %llx\n", m_opRegisters->crcr);

        msleep(100);

        // Poll the event ring for a completion event
        kuPrint("[XHCI] Polling Event Ring at address: %llx\n", eventRing);

        while (true) {
            bool found = false; 

            for (uint32_t i = 0; i < eventRingTRBs; ++i) {
                if (eventRing[i].control != 0) {
                    kuPrint("[XHCI] Event Ring TRB[%i]: parameter = %llx, status = %x, control = %x\n",
                            i, eventRing[i].parameter, eventRing[i].status, eventRing[i].control);
                    
                    // if ((eventRing[i].control >> 10) == XHCI_TRB_TYPE_ENABLE_SLOT_CMD) {
                    //     kuPrint("[XHCI] Enable Slot TRB found!\n");
                    // }

                    // if ((eventRing[i].control >> 10) == XHCI_TRB_TYPE_NOOP_CMD) {
                    //     kuPrint("[XHCI] NOOP TRB found!\n");
                    // }

                    found = true;
                }
            }

            if (found) break;
        }

        kuPrint("\n");
        return true;
    }

    void XhciDriver::_mapDeviceMmio(uint64_t pciBarAddress) {
        // Map a conservatively large space for xHCI registers
        for (size_t offset = 0; offset < 0x20000; offset += PAGE_SIZE) {
            void* mmioPage = (void*)(pciBarAddress + offset);
            paging::mapPage(mmioPage, mmioPage, KERNEL_PAGE, PAGE_ATTRIB_CACHE_DISABLED, paging::g_kernelRootPageTable);
        }

        paging::flushTlbAll();

        m_xhcBase = pciBarAddress;
        m_capRegisters = (volatile XhciCapabilityRegisters*)pciBarAddress;
    }

    void* XhciDriver::_allocXhciMemory(size_t size) {
        void* ptr = kmallocAligned(size, 64);
        if (!ptr) {
            kuPrint("[XHCI] ======= MEMORY ALLOCATION PROBLEM =======\n");
            
            // Ideally panic, but spin for now
            while (true);
        }

        // Make sure the memory is uncacheable
        paging::markPageUncacheable(ptr);

        return ptr;
    }

    void XhciDriver::_assumeOwnershipFromBios() {
        // Read the USB Legacy Support Capability Register
        uint32_t legacySupportCap = _readExtendedCapability(XHCI_LEGACY_SUPPORT_CAP_ID);
        if (legacySupportCap == 0) {
            kuPrint("[XHCI] Legacy support capability not found\n");
            return;
        }

        // Clear BIOS owned semaphore
        legacySupportCap &= ~XHCI_LEGACY_BIOS_OWNED_SEMAPHORE;
        _writeExtendedCapability(XHCI_LEGACY_SUPPORT_CAP_ID, legacySupportCap);

        // Set OS owned semaphore
        legacySupportCap |= XHCI_LEGACY_OS_OWNED_SEMAPHORE;
        _writeExtendedCapability(XHCI_LEGACY_SUPPORT_CAP_ID, legacySupportCap);

        // Wait for the BIOS to release control
        while (_readExtendedCapability(XHCI_LEGACY_SUPPORT_CAP_ID) & XHCI_LEGACY_BIOS_OWNED_SEMAPHORE) {
            msleep(16);
        }

        // Clear SMI enable bits in the USB Legacy Support Control/Status register
        uint32_t usblegcctlsts = _readExtendedCapability(XHCI_LEGACY_SUPPORT_CAP_ID + 1);
        if (usblegcctlsts == 0) {
            kuPrint("[XHCI] USB Legacy Support Control/Status register capability not found\n");
        } else {
            usblegcctlsts &= ~XHCI_LEGACY_SMI_ENABLE_BITS;
            _writeExtendedCapability(XHCI_LEGACY_SUPPORT_CAP_ID + 1, usblegcctlsts);
        }
    }

    void XhciDriver::_writeUsbRegCommand(uint32_t cmd) {
        m_opRegisters->usbcmd |= cmd;
    }
    
    bool XhciDriver::_readUsbRegStatusFlag(uint32_t flag) {
        return (m_opRegisters->usbsts & flag);
    }

    uint32_t XhciDriver::_readExtendedCapability(uint8_t capId) {
        uint32_t offset = (XHCI_XECP(m_capRegisters) << 2);
        while (offset) {
            volatile uint32_t* capPtr = (volatile uint32_t*)((uint64_t)m_capRegisters + offset);
            uint8_t id = *capPtr & 0xFF;
            if (id == capId) {
                return *capPtr;
            }
            offset = (*capPtr >> 8) & 0xFF; // Next capability offset
            offset <<= 2; // Convert to byte offset
        }

        kuPrint("[XHCI] Warning: extended capability 0x%x not found\n", (int)capId);
        return 0; // Capability not found
    }

    void XhciDriver::_writeExtendedCapability(uint8_t capId, uint32_t value) {
        uint32_t offset = (XHCI_XECP(m_capRegisters) << 2);
        while (offset) {
            volatile uint32_t* capPtr = (volatile uint32_t*)((uint64_t)m_capRegisters + offset);
            uint8_t id = *capPtr & 0xFF;
            if (id == capId) {
                *capPtr = value;
                return;
            }
            offset = (*capPtr >> 8) & 0xFF; // Next capability offset
            offset <<= 2; // Convert to byte offset
        }

        kuPrint("[XHCI] Warning: extended capability 0x%x not found\n", (int)capId);
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

    volatile uint32_t* XhciDriver::_getImanRegAddress(uint32_t interrupter) {
        // Address: Runtime Base + 020h + (32 * Interrupter)
        //          where: Interrupter is 0, 1, 2, 3, … 1023
        uint64_t base = m_runtimeRegisterBase + 0x20 + (32 * interrupter);

        return (volatile uint32_t*)base;
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
        bool error = m_opRegisters->usbsts & XHCI_USBSTS_HCE;
        if (error) {
            kuPrint("[XHCI] ==== Host Controller Error ====\n");
            kuPrint("       USBCMD: 0x%llx\n", m_opRegisters->usbcmd);
            kuPrint("       USBSTS: 0x%llx\n", m_opRegisters->usbsts);
            kuPrint("\n");
        }

        return error;
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
} // namespace drivers
