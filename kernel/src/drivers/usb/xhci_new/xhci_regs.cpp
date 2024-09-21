#include "xhci_regs.h"

const char* xhciExtendedCapabilityToString(XhciExtendedCapabilityCode capid) {
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

XhciInterrupterRegisters* XhciRuntimeRegisterManager::getInterrupterRegisters(uint8_t interrupter) const {
    if (interrupter > m_maxInterrupters) {
        return nullptr;
    }

    return &m_base->ir[interrupter];
}

XhciDoorbellManager::XhciDoorbellManager(uint64_t base) {
    m_doorbellRegisters = reinterpret_cast<XhciDoorbellRegister*>(base);
}

void XhciDoorbellManager::ringDoorbell(uint8_t doorbell, uint8_t target) {
    m_doorbellRegisters[doorbell].raw = static_cast<uint32_t>(target);
}

void XhciDoorbellManager::ringCommandDoorbell() {
    ringDoorbell(0, XHCI_DOORBELL_TARGET_COMMAND_RING);
}

void XhciDoorbellManager::ringControlEndpointDoorbell(uint8_t doorbell) {
    ringDoorbell(doorbell, XHCI_DOORBELL_TARGET_CONTROL_EP_RING);
}

XhciExtendedCapability::XhciExtendedCapability(volatile uint32_t* capPtr) : m_base(capPtr) {
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
