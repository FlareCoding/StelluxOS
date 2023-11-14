#include "xhci.h"
#include <paging/page.h>
#include <paging/phys_addr_translation.h>
#include <memory/kmemory.h>
#include <kprint.h>

#define XHCI_MAX_SLOTS(regs) ((regs->hcsparams1) & 0xFF)
#define XHCI_NUM_PORTS(regs) ((regs->hcsparams1 >> 24) & 0xFF)

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

XhciCapabilityRegisters* mapXhciRegisters(uint64_t bar) {
    // Map a conservatively large space for xHCI registers
    for (size_t offset = 0; offset < 0x20000; offset += PAGE_SIZE) {
        paging::mapPage((void*)(bar + offset), (void*)(bar + offset), USERSPACE_PAGE, paging::g_kernelRootPageTable);
    }

    return (XhciCapabilityRegisters*)bar;
}

// bool resetXhciController(XhciOperationalRegisters* regs) {
//     // Write to USB Command Register to reset the controller
//     regs->usbcmd |= XHCI_CMD_RESET;

//     // Wait for the reset bit to be cleared, indicating completion
//     uint64_t timeout = 0x600000ULL;
//     while (regs->usbcmd & XHCI_CMD_RESET) {
//         if (timeout == 0) break;
//         --timeout;
//     }

//     // After reset, ensure that the Run/Stop bit is cleared
//     if (regs->usbcmd & XHCI_CMD_RUN) {
//         return false;
//     }

//     // Check the USB Status Register for Controller Not Ready (CNR) status
//     if (regs->usbsts & XHCI_STS_CNR) {
//         kuPrint("Xhci controller didn't have XHCI_STS_CNR bit set\n");
//         return false;
//     }

//     return true;
// }

void xhciControllerInit(size_t bar) {
    // Map the xHCI registers
    XhciCapabilityRegisters* capabilityRegs = mapXhciRegisters(bar);

    uint32_t maxDeviceSlots = XHCI_MAX_SLOTS(capabilityRegs);
    uint32_t numPorts = XHCI_NUM_PORTS(capabilityRegs);

    uint64_t opRegBase = (uint64_t)capabilityRegs + capabilityRegs->caplength;
    XhciOperationalRegisters* operationalRegs = (XhciOperationalRegisters*)opRegBase;

    printXhciCapabilityRegisters(capabilityRegs);
    printXhciOperationalRegisters(operationalRegs);

    kuPrint("System has %i ports and %i device slots\n", numPorts, maxDeviceSlots);
}
