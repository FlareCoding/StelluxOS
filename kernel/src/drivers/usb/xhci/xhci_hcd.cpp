#include "xhci_hcd.h"
#include <acpi/mcfg.h>
#include <kprint.h>

XhciHcContext::XhciHcContext(uint64_t xhcBase) {
    m_capRegs = reinterpret_cast<volatile XhciCapabilityRegisters*>(xhcBase);
    m_opRegs = reinterpret_cast<volatile XhciOperationalRegisters*>(xhcBase + m_capRegs->caplength);

    // Enable device notifications 
    m_opRegs->dnctrl = 0xffff;

    // Configure the usbconfig field
    m_opRegs->config = static_cast<uint32_t>(getMaxDeviceSlots());

    dumpCapabilityRegisters();
}

uint8_t XhciHcContext::getMaxDeviceSlots() {
    return XHCI_MAX_DEVICE_SLOTS(m_capRegs);
}

uint8_t XhciHcContext::getMaxInterrupters() {
    return XHCI_MAX_INTERRUPTERS(m_capRegs);
}

uint8_t XhciHcContext::getMaxPorts() {
    return XHCI_MAX_PORTS(m_capRegs);
}

uint8_t XhciHcContext::getIsochronousSchedulingThreshold() {
    return XHCI_IST(m_capRegs);
}

uint8_t XhciHcContext::getErstMax() {
    return XHCI_ERST_MAX(m_capRegs);
}

uint8_t XhciHcContext::getMaxScratchpadBuffers() {
    return XHCI_MAX_SCRATCHPAD_BUFFERS(m_capRegs);
}

bool XhciHcContext::is64bitAddressable() {
    return XHCI_AC64(m_capRegs);
}

bool XhciHcContext::hasBandwidthNegotiationCapability() {
    return XHCI_BNC(m_capRegs);
}

bool XhciHcContext::has64ByteContextSize() {
    return XHCI_CSZ(m_capRegs);
}

bool XhciHcContext::hasPortPowerControl() {
    return XHCI_PPC(m_capRegs);
}

bool XhciHcContext::hasPortIndicators() {
    return XHCI_PIND(m_capRegs);
}

bool XhciHcContext::hasLightResetCapability() {
    return XHCI_LHRC(m_capRegs);
}


uint32_t XhciHcContext::getExtendedCapabilitiesOffset() {
    return XHCI_XECP(m_capRegs) * sizeof(uint32_t);
}


uint64_t XhciHcContext::getXhcPageSize() {
    return static_cast<uint64_t>(m_opRegs->pagesize & 0xffff) << 12;
}


void XhciHcContext::dumpCapabilityRegisters() {
    kprintInfo("===== Capability Registers (0x%llx) =====\n", (uint64_t)m_capRegs);
    kprintInfo("    Length                : %i\n", m_capRegs->caplength);
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

void XhciHcd::init(PciDeviceInfo* deviceInfo) {
    uint64_t xhcBase = xhciMapMmio(deviceInfo->barAddress);

    m_ctx = kstl::SharedPtr<XhciHcContext>(new XhciHcContext(xhcBase));
}
