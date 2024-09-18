#include "xhci_ctx.h"

#include <kprint.h>

XhciHcContext::XhciHcContext(uint64_t xhcBase) {
    this->xhcBase = xhcBase;

    capRegs = reinterpret_cast<volatile XhciCapabilityRegisters*>(xhcBase);
    opRegs = reinterpret_cast<volatile XhciOperationalRegisters*>(xhcBase + capRegs->caplength);
    runtimeRegs = reinterpret_cast<volatile XhciRuntimeRegisters*>(xhcBase + capRegs->rtsoff);

    // Read extended capabilities
    volatile uint32_t* headCapPtr = reinterpret_cast<volatile uint32_t*>(
        xhcBase + getExtendedCapabilitiesOffset()
    );

    extendedCapabilitiesHead = kstl::SharedPtr<XhciExtendedCapability>(
        new XhciExtendedCapability(headCapPtr)
    );

    // Construct a manager class instance for the doorbell register array
    doorbellManager = kstl::SharedPtr<XhciDoorbellManager>(
        new XhciDoorbellManager(xhcBase + capRegs->dboff)
    );

    // Allocate a command ring
    commandRing = kstl::SharedPtr<XhciCommandRing>(
        new XhciCommandRing(XHCI_COMMAND_RING_TRB_COUNT)
    );

    // Pre-allocate kstl::vector capacity for enough ports
    usb3Ports.reserve(getMaxPorts());

    dumpCapabilityRegisters();
}

uint8_t XhciHcContext::getMaxDeviceSlots() const {
    return XHCI_MAX_DEVICE_SLOTS(capRegs);
}

uint8_t XhciHcContext::getMaxInterrupters() const {
    return XHCI_MAX_INTERRUPTERS(capRegs);
}

uint8_t XhciHcContext::getMaxPorts() const {
    return XHCI_MAX_PORTS(capRegs);
}

uint8_t XhciHcContext::getIsochronousSchedulingThreshold() const {
    return XHCI_IST(capRegs);
}

uint8_t XhciHcContext::getErstMax() const {
    return XHCI_ERST_MAX(capRegs);
}

uint8_t XhciHcContext::getMaxScratchpadBuffers() const {
    return XHCI_MAX_SCRATCHPAD_BUFFERS(capRegs);
}

bool XhciHcContext::is64bitAddressable() const {
    return XHCI_AC64(capRegs);
}

bool XhciHcContext::hasBandwidthNegotiationCapability() const {
    return XHCI_BNC(capRegs);
}

bool XhciHcContext::has64ByteContextSize() const {
    return XHCI_CSZ(capRegs);
}

bool XhciHcContext::hasPortPowerControl() const {
    return XHCI_PPC(capRegs);
}

bool XhciHcContext::hasPortIndicators() const {
    return XHCI_PIND(capRegs);
}

bool XhciHcContext::hasLightResetCapability() const {
    return XHCI_LHRC(capRegs);
}


uint32_t XhciHcContext::getExtendedCapabilitiesOffset() const {
    return XHCI_XECP(capRegs) * sizeof(uint32_t);
}


uint64_t XhciHcContext::getXhcPageSize() const {
    return static_cast<uint64_t>(opRegs->pagesize & 0xffff) << 12;
}

bool XhciHcContext::isPortUsb3(uint8_t port) {
    for (size_t i = 0; i < usb3Ports.size(); ++i) {
        if (usb3Ports[i] == port) {
            return true;
        }
    }

    return false;
}

XhciPortRegisterManager XhciHcContext::getPortRegisterSet(uint8_t port) {
    uint64_t base = reinterpret_cast<uint64_t>(opRegs) + (0x400 + (0x10 * port));
    return XhciPortRegisterManager(base);
}

void XhciHcContext::dumpCapabilityRegisters() {
    kprintInfo("===== Capability Registers (0x%llx) =====\n", (uint64_t)capRegs);
    kprintInfo("    Length                : %i\n", capRegs->caplength);
    kprintInfo("    Max Device Slots      : %i\n", getMaxDeviceSlots());
    kprintInfo("    Max Interrupters      : %i\n", getMaxInterrupters());
    kprintInfo("    Max Ports             : %i\n", getMaxPorts());
    kprintInfo("    IST                   : %i\n", getIsochronousSchedulingThreshold());
    kprintInfo("    ERST Max Size         : %i\n", getErstMax());
    kprintInfo("    Scratchpad Buffers    : %i\n", getMaxScratchpadBuffers());
    kprintInfo("    64-bit Addressing     : %i\n", is64bitAddressable());
    kprintInfo("    Bandwidth Negotiation : %i\n", hasBandwidthNegotiationCapability());
    kprintInfo("    64-byte Context Size  : %i\n", has64ByteContextSize());
    kprintInfo("    Port Power Control    : %i\n", hasPortPowerControl());
    kprintInfo("    Port Indicators       : %i\n", hasPortIndicators());
    kprintInfo("    Light Reset Available : %i\n", hasLightResetCapability());
    kprint("\n");
}
