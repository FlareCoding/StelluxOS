#include <drivers/usb/xhci/xhci_regs.h>

const char* xhci_extended_capability_to_string(xhci_extended_capability_code capid) {
    uint8_t id = static_cast<uint8_t>(capid);

    switch (capid) {
    case xhci_extended_capability_code::reserved: return "Reserved";
    case xhci_extended_capability_code::usb_legacy_support: return "USB Legacy Support";
    case xhci_extended_capability_code::supported_protocol: return "Supported Protocol";
    case xhci_extended_capability_code::extended_power_management: return "Extended Power Management";
    case xhci_extended_capability_code::iovirtualization_support: return "I/O Virtualization Support";
    case xhci_extended_capability_code::local_memory_support: return "Local Memory Support";
    case xhci_extended_capability_code::usb_debug_capability_support: return "USB Debug Capability Support";
    case xhci_extended_capability_code::extended_message_interrupt_support: return "Extended Message Interrupt Support";
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

xhci_doorbell_manager::xhci_doorbell_manager(uintptr_t base) {
    m_doorbell_registers = reinterpret_cast<xhci_doorbell_register*>(base);
}

void xhci_doorbell_manager::ring_doorbell(uint8_t doorbell, uint8_t target) {
    m_doorbell_registers[doorbell].raw = static_cast<uint32_t>(target);
}

void xhci_doorbell_manager::ring_command_doorbell() {
    ring_doorbell(0, XHCI_DOORBELL_TARGET_COMMAND_RING);
}

void xhci_doorbell_manager::ring_control_endpoint_doorbell(uint8_t doorbell) {
    ring_doorbell(doorbell, XHCI_DOORBELL_TARGET_CONTROL_EP_RING);
}

xhci_extended_capability::xhci_extended_capability(volatile uint32_t* capPtr) : m_base(capPtr) {
    m_entry.raw = *m_base;
    _read_next_ext_caps();
}

void xhci_extended_capability::_read_next_ext_caps() {
    if (m_entry.next) {
        auto next_cap_ptr = XHCI_NEXT_EXT_CAP_PTR(m_base, m_entry.next);
        m_next = kstl::shared_ptr<xhci_extended_capability>(
            new xhci_extended_capability(next_cap_ptr)
        );
    }
}

void xhci_port_register_manager::read_portsc_reg(xhci_portsc_register& reg) const {
    uint64_t portsc_address = m_base + m_portsc_offset;
    reg.raw = *reinterpret_cast<volatile uint32_t*>(portsc_address);
}

void xhci_port_register_manager::write_portsc_reg(xhci_portsc_register& reg) const {
    uint64_t portsc_address = m_base + m_portsc_offset;
    *reinterpret_cast<volatile uint32_t*>(portsc_address) = reg.raw;
}

void xhci_port_register_manager::read_portpmsc_reg_usb2(xhci_portpmsc_register_usb2& reg) const {
    uint64_t portpmsc_address = m_base + m_portpmsc_offset;
    reg.raw = *reinterpret_cast<volatile uint32_t*>(portpmsc_address);
}

void xhci_port_register_manager::write_portpmsc_reg_usb2(xhci_portpmsc_register_usb2& reg) const {
    uint64_t portpmsc_address = m_base + m_portpmsc_offset;
    *reinterpret_cast<volatile uint32_t*>(portpmsc_address) = reg.raw;
}

void xhci_port_register_manager::read_portpmsc_reg_usb3(xhci_portpmsc_register_usb3& reg) const {
    uint64_t portpmsc_address = m_base + m_portpmsc_offset;
    reg.raw = *reinterpret_cast<volatile uint32_t*>(portpmsc_address);
}

void xhci_port_register_manager::write_portpmsc_reg_usb3(xhci_portpmsc_register_usb3& reg) const {
    uint64_t portpmsc_address = m_base + m_portpmsc_offset;
    *reinterpret_cast<volatile uint32_t*>(portpmsc_address) = reg.raw;
}

void xhci_port_register_manager::read_portli_reg(xhci_portli_register& reg) const {
    uint64_t portli_address = m_base + m_portli_offset;
    reg.raw = *reinterpret_cast<volatile uint32_t*>(portli_address);
}

void xhci_port_register_manager::write_portli_reg(xhci_portli_register& reg) const {
    uint64_t portli_address = m_base + m_portli_offset;
    *reinterpret_cast<volatile uint32_t*>(portli_address) = reg.raw;
}

void xhci_port_register_manager::read_porthlpmc_reg_usb2(xhci_porthlpmc_register_usb2& reg) const {
    uint64_t porthlpm_address = m_base + m_porthlpmc_offset;
    reg.raw = *reinterpret_cast<volatile uint32_t*>(porthlpm_address);
}

void xhci_port_register_manager::write_porthlpmc_reg_usb2(xhci_porthlpmc_register_usb2& reg) const {
    uint64_t porthlpm_address = m_base + m_porthlpmc_offset;
    *reinterpret_cast<volatile uint32_t*>(porthlpm_address) = reg.raw;
}

void xhci_port_register_manager::read_porthlpmc_reg_usb3(xhci_porthlpmc_register_usb3& reg) const {
    uint64_t porthlpm_address = m_base + m_porthlpmc_offset;
    reg.raw = *reinterpret_cast<volatile uint32_t*>(porthlpm_address);
}

void xhci_port_register_manager::write_porthlpmc_reg_usb3(xhci_porthlpmc_register_usb3& reg) const {
    uint64_t porthlpm_address = m_base + m_porthlpmc_offset;
    *reinterpret_cast<volatile uint32_t*>(porthlpm_address) = reg.raw;
}
