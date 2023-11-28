#include "xhci.h"
#include <paging/page.h>
#include <paging/phys_addr_translation.h>
#include <memory/kmemory.h>
#include <time/ktime.h>
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

    bool XhciDriver::init(uint64_t pciBarAddress) {
        _mapDeviceMmio(pciBarAddress);

        uint64_t opRegBase = (uint64_t)m_capRegisters + m_capRegisters->caplength;
        m_opRegisters = (volatile XhciOperationalRegisters*)opRegBase;

        uint64_t runtimeRegBase = opRegBase + m_capRegisters->rtsoff;
        m_rtRegisters = (volatile XhciRuntimeRegisters*)runtimeRegBase;

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

        kuPrint("\n\n");
        // printXhciCapabilityRegisters(m_capRegisters);
        printXhciOperationalRegisters(m_opRegisters);

        kuPrint("System has %i ports and %i device slots\n", m_numPorts, m_maxDeviceSlots);

        // Enable the controller
        _enableController();

        // Hot reset the ports
        _resetPorts();

        while (true) {
            for (uint32_t i = 1; i <= m_numPorts; i++) {
                XhciPortscRegister portscReg;
                _readPortscReg(i, portscReg);
                kuPrint("%i ", portscReg.bits.ccs);
            }
            kuPrint("\n");

            sleep(1);
        }

        return true;
    }

    void XhciDriver::_mapDeviceMmio(uint64_t pciBarAddress) {
        // Map a conservatively large space for xHCI registers
        for (size_t offset = 0; offset < 0x20000; offset += PAGE_SIZE) {
            paging::mapPage((void*)(pciBarAddress + offset), (void*)(pciBarAddress + offset), USERSPACE_PAGE, paging::g_kernelRootPageTable);
        }

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
        XhciDeviceContext* firstDeviceContext = (XhciDeviceContext*)kmallocAligned(sizeof(XhciDeviceContext), 64);
        if (!firstDeviceContext) {
            return false;
        }

        zeromem(firstDeviceContext, sizeof(XhciDeviceContext));

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

        dcbaap[1] = firstDeviceContext;

        return true;
    }

    void XhciDriver::_readPortscReg(uint32_t portNum, XhciPortscRegister& reg) {
        // Operational Base + (400h + (10h * (n – 1)))
        uint64_t opbase = (uint64_t)m_opRegisters;
        uint64_t portscBase = opbase + (0x400 + (0x10 * (portNum - 1)));

        reg.raw = ((volatile XhciPortscRegister*)portscBase)->raw;
    }

    void XhciDriver::_writePortscReg(uint32_t portNum, XhciPortscRegister& reg) {
        // Operational Base + (400h + (10h * (n – 1)))
        uint64_t opbase = (uint64_t)m_opRegisters;
        uint64_t portscBase = opbase + (0x400 + (0x10 * (portNum - 1)));

        ((volatile XhciPortscRegister*)portscBase)->raw = reg.raw;
    }

    void XhciDriver::_resetPort(uint32_t portNum) {
        XhciPortscRegister portscReg;

        _readPortscReg(portNum, portscReg);
        portscReg.bits.pr = 1;
        _writePortscReg(portNum, portscReg);

        do {
            _readPortscReg(portNum, portscReg);
            msleep(10);
        } while (portscReg.bits.pr);

        _readPortscReg(portNum, portscReg);
        portscReg.bits.prc = 1;
        _writePortscReg(portNum, portscReg);
    }

    void XhciDriver::_resetPorts() {
        for (uint32_t i = 1; i <= m_numPorts; i++) {
            _resetPort(i);
        }
    }
} // namespace drivers
