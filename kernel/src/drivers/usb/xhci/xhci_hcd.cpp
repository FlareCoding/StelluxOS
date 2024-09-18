#include "xhci_hcd.h"
#include <acpi/mcfg.h>
#include <time/ktime.h>
#include <kprint.h>

#include "xhci_ext_cap.h"

void XhciHcd::init(PciDeviceInfo* deviceInfo) {
    uint64_t xhcBase = xhciMapMmio(deviceInfo->barAddress);

    // Create the host controller context data structure
    m_ctx = kstl::SharedPtr<XhciHcContext>(new XhciHcContext(xhcBase));

    // Create a device context manager
    m_deviceContextManager = kstl::SharedPtr<XhciDeviceContextManager>(new XhciDeviceContextManager());

    // Map which port register sets are USB2 and which are USB3
    _identifyUsb3Ports();

    // Reset the controller's internal state
    if (!resetController()) {
        // TO-DO: handle error appropriately
        return;
    }

    // Configure operational registers
    _configureOperationalRegs();

    // Setup the event ring and interrupter registers
    m_ctx->eventRing = kstl::SharedPtr<XhciEventRing>(
        new XhciEventRing(XHCI_EVENT_RING_TRB_COUNT, &m_ctx->runtimeRegs->ir[0])
    );

    kprint("[XHCI] DCBAAP   : 0x%llx\n", m_ctx->opRegs->dcbaap);
    kprint("[XHCI] CRCR     : 0x%llx\n", m_ctx->opRegs->crcr);
    kprint("[XHCI] ERSTSZ   : %lli\n", m_ctx->runtimeRegs->ir[0].erstsz);
    kprint("[XHCI] ERDP     : 0x%llx\n", m_ctx->runtimeRegs->ir[0].erdp);
    kprint("[XHCI] ERSTBA   : 0x%llx\n", m_ctx->runtimeRegs->ir[0].erstba);
    kprint("\n");

    // Start the controller
    startController();

    // Reset the ports
    resetAllPorts();
}

bool XhciHcd::resetController() {
    // Make sure we clear the Run/Stop bit
    uint32_t usbcmd = m_ctx->opRegs->usbcmd;
    usbcmd &= ~XHCI_USBCMD_RUN_STOP;
    m_ctx->opRegs->usbcmd = usbcmd;

    // Wait for the HCHalted bit to be set
    uint32_t timeout = 20;
    while (!(m_ctx->opRegs->usbsts & XHCI_USBSTS_HCH)) {
        if (--timeout == 0) {
            kprint("XHCI HC did not halt within %ims\n", timeout);
            return false;
        }

        msleep(1);
    }

    // Set the HC Reset bit
    usbcmd = m_ctx->opRegs->usbcmd;
    usbcmd |= XHCI_USBCMD_HCRESET;
    m_ctx->opRegs->usbcmd = usbcmd;

    // Wait for this bit and CNR bit to clear
    timeout = 100;
    while (
        m_ctx->opRegs->usbcmd & XHCI_USBCMD_HCRESET ||
        m_ctx->opRegs->usbsts & XHCI_USBSTS_CNR
    ) {
        if (--timeout == 0) {
            kprint("XHCI HC did not reset within %ims\n", timeout);
            return false;
        }

        msleep(1);
    }

    msleep(50);

    // Check the defaults of the operational registers
    if (m_ctx->opRegs->usbcmd != 0)
        return false;

    if (m_ctx->opRegs->dnctrl != 0)
        return false;

    if (m_ctx->opRegs->crcr != 0)
        return false;

    if (m_ctx->opRegs->dcbaap != 0)
        return false;

    if (m_ctx->opRegs->config != 0)
        return false;

    return true;
}

void XhciHcd::startController() {
    uint32_t usbcmd = m_ctx->opRegs->usbcmd;
    usbcmd |= XHCI_USBCMD_RUN_STOP;
    usbcmd |= XHCI_USBCMD_INTERRUPTER_ENABLE;
    usbcmd |= XHCI_USBCMD_HOSTSYS_ERROR_ENABLE;

    m_ctx->opRegs->usbcmd = usbcmd;

    // Make sure the controller's HCH flag is cleared
    while (m_ctx->opRegs->usbsts & XHCI_USBSTS_HCH) {
        msleep(16);
    }
}

bool XhciHcd::resetPort(uint8_t port) {
    XhciPortRegisterManager regset = m_ctx->getPortRegisterSet(port);
    XhciPortscRegister portsc;
    regset.readPortscReg(portsc);

    bool isUsb3Port = m_ctx->isPortUsb3(port);

    if (portsc.pp == 0) {
        portsc.pp = 1;
        regset.writePortscReg(portsc);
        msleep(20);
        regset.readPortscReg(portsc);

        if (portsc.pp == 0) {
            kprintWarn("Port %i: Bad Reset\n", port);
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

    int timeout = 60;
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

void XhciHcd::resetAllPorts() {
    for (uint8_t i = 0; i < m_ctx->getMaxPorts(); i++) {
        if (resetPort(i)) {
            kprintInfo("[*] Successfully reset %s port %i\n", m_ctx->isPortUsb3(i) ? "USB3" : "USB2", i);
        } else {
            kprintWarn("[*] Failed to reset %s port %i\n", m_ctx->isPortUsb3(i) ? "USB3" : "USB2", i);
        }
    }
    kprint("\n");
}

void XhciHcd::clearIrqFlags(uint8_t interrupter) {
    // Get the interrupter registers
    auto interrupterRegs = &m_ctx->runtimeRegs->ir[interrupter];

    // Clear the interrupt pending bit in the primary interrupter
    interrupterRegs->iman |= ~XHCI_IMAN_INTERRUPT_PENDING;

    // Clear the interrupt pending bit in USBSTS
    m_ctx->opRegs->usbsts |= ~XHCI_USBSTS_EINT;
}

void XhciHcd::_logUsbsts() {
    uint32_t status = m_ctx->opRegs->usbsts;
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

void XhciHcd::_identifyUsb3Ports() {
    auto node = m_ctx->extendedCapabilitiesHead;
    while (node.get()) {
        if (node->id() == XhciExtendedCapabilityCode::SupportedProtocol) {
            XhciUsbSupportedProtocolCapability cap(node->base());
            // Make the ports zero-based
            uint8_t firstPort = cap.compatiblePortOffset - 1;
            uint8_t lastPort = firstPort + cap.compatiblePortCount - 1;

            if (cap.majorRevisionVersion == 3) {
                for (uint8_t port = firstPort; port <= lastPort; port++) {
                    m_ctx->usb3Ports.pushBack(port);
                }
            }
        }

        node = node->next();
    }
}

void XhciHcd::_configureOperationalRegs() {
    // Enable device notifications 
    m_ctx->opRegs->dnctrl = 0xffff;

    // Configure the usbconfig field
    m_ctx->opRegs->config = static_cast<uint32_t>(m_ctx->getMaxDeviceSlots());

    // Allocate the set the DCBAA pointer
    m_deviceContextManager->allocateDcbaa(m_ctx.get());

    // Write the CRCR register
    m_ctx->opRegs->crcr = m_ctx->commandRing->getPhysicalBase();
}
