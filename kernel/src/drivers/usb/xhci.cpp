#include "xhci.h"
#include <paging/page.h>
#include <paging/phys_addr_translation.h>
#include <memory/kmemory.h>
#include <time/ktime.h>
#include <kprint.h>

namespace drivers {

    XhciDriver g_globalXhciInstance;

    void printXhciCapabilityRegisters(XhciCapabilityRegisters* capRegs) {
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

    void printXhciOperationalRegisters(XhciOperationalRegisters* opRegs) {
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

    bool XhciDriver::init(uint64_t pciBarAddress) {
        _mapDeviceMmio(pciBarAddress);

        uint32_t maxDeviceSlots = XHCI_MAX_DEVICE_SLOTS(m_capRegisters);
        uint32_t numPorts = XHCI_NUM_PORTS(m_capRegisters);

        uint64_t opRegBase = (uint64_t)m_capRegisters + m_capRegisters->caplength;
        m_opRegisters = (XhciOperationalRegisters*)opRegBase;

        printXhciCapabilityRegisters(m_capRegisters);
        printXhciOperationalRegisters(m_opRegisters);

        _resetController();

        kuPrint("\n\n");
        printXhciCapabilityRegisters(m_capRegisters);
        printXhciOperationalRegisters(m_opRegisters);

        kuPrint("System has %i ports and %i device slots\n", numPorts, maxDeviceSlots);

        return true;
    }

    void XhciDriver::_mapDeviceMmio(uint64_t pciBarAddress) {
        // Map a conservatively large space for xHCI registers
        for (size_t offset = 0; offset < 0x20000; offset += PAGE_SIZE) {
            paging::mapPage((void*)(pciBarAddress + offset), (void*)(pciBarAddress + offset), USERSPACE_PAGE, paging::g_kernelRootPageTable);
        }

        m_capRegisters = (XhciCapabilityRegisters*)pciBarAddress;
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

    void XhciDriver::_resetController() {
        _writeUsbRegCommand(XHCI_USBCMD_HCRESET);

        while (!_isControllerReady()) {
            msleep(1);
        }
    }
} // namespace drivers
