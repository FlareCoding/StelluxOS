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

    const char* extendedCapabilityToString(XhciExtendedCapabilityCode capid) {
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

    XhciExtendedCapability::XhciExtendedCapability(volatile uint32_t* capPtr)
    : m_base(capPtr) {
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

    XhciDriver& XhciDriver::get() {
        return g_globalXhciInstance;
    }

    bool XhciDriver::init(PciDeviceInfo& deviceInfo) {
        _mapDeviceMmio(deviceInfo.barAddress);

        _parseCapabilityRegisters();
        _logCapabilityRegisters();

        _parseExtendedCapabilityRegisters();

        _logUsbsts();

        kprint("\n");
        return true;
    }

    void XhciDriver::_parseCapabilityRegisters() {
        m_capRegs = reinterpret_cast<volatile XhciCapabilityRegisters*>(m_xhcBase);

        m_capabilityRegsLength = m_capRegs->caplength;

        m_maxDeviceSlots = XHCI_MAX_DEVICE_SLOTS(m_capRegs);
        m_maxInterrupters = XHCI_MAX_INTERRUPTERS(m_capRegs);
        m_maxPorts = XHCI_MAX_PORTS(m_capRegs);

        m_isochronousSchedulingThreshold = XHCI_IST(m_capRegs);
        m_erstMax = XHCI_ERST_MAX(m_capRegs);
        m_maxScratchpadBuffers = XHCI_MAX_SCRATCHPAD_BUFFERS(m_capRegs);

        m_64bitAddressingCapability = XHCI_AC64(m_capRegs);
        m_bandwidthNegotiationCapability = XHCI_BNC(m_capRegs);
        m_64ByteContextSize = XHCI_CSZ(m_capRegs);
        m_portPowerControl = XHCI_PPC(m_capRegs);
        m_portIndicators = XHCI_PIND(m_capRegs);
        m_lightResetCapability = XHCI_LHRC(m_capRegs);
        m_extendedCapabilitiesOffset = XHCI_XECP(m_capRegs) * sizeof(uint32_t);

        // Update the base pointer to operational register set
        m_opRegs = reinterpret_cast<volatile XhciOperationalRegisters*>(m_xhcBase + m_capabilityRegsLength);
    }

    void XhciDriver::_logCapabilityRegisters() {
        kprintInfo("===== Capability Registers (0x%llx) =====\n", (uint64_t)m_capRegs);
        kprintInfo("    Length                : %i\n", m_capabilityRegsLength);
        kprintInfo("    Max Device Slots      : %i\n", m_maxDeviceSlots);
        kprintInfo("    Max Interrupters      : %i\n", m_maxInterrupters);
        kprintInfo("    Max Ports             : %i\n", m_maxPorts);
        kprintInfo("    IST                   : %i\n", m_isochronousSchedulingThreshold);
        kprintInfo("    ERST Max Size         : %i\n", m_erstMax);
        kprintInfo("    Scratchpad Buffers    : %i\n", m_maxScratchpadBuffers);
        kprintInfo("    64-bit Addressing     : %i\n", m_64bitAddressingCapability);
        kprintInfo("    Bandwidth Negotiation : %i\n", m_bandwidthNegotiationCapability);
        kprintInfo("    64-byte Context Size  : %i\n", m_64ByteContextSize);
        kprintInfo("    Port Power Control    : %i\n", m_portPowerControl);
        kprintInfo("    Port Indicators       : %i\n", m_portIndicators);
        kprintInfo("    Light Reset Available : %i\n", m_lightResetCapability);
        kprint("\n");
    }

    void XhciDriver::_parseExtendedCapabilityRegisters() {
        volatile uint32_t* headCapPtr = reinterpret_cast<volatile uint32_t*>(
            m_xhcBase + m_extendedCapabilitiesOffset
        );

        m_extendedCapabilitiesHead = kstl::SharedPtr<XhciExtendedCapability>(
            new XhciExtendedCapability(headCapPtr)
        );
    }

    void XhciDriver::_logUsbsts() {
        uint32_t status = m_opRegs->usbsts;
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

    void XhciDriver::_mapDeviceMmio(uint64_t pciBarAddress) {
        // Map a conservatively large space for xHCI registers
        for (size_t offset = 0; offset < 0x20000; offset += PAGE_SIZE) {
            void* mmioPage = (void*)(pciBarAddress + offset);
            paging::mapPage(mmioPage, mmioPage, KERNEL_PAGE, PAGE_ATTRIB_CACHE_DISABLED, paging::g_kernelRootPageTable);
        }

        paging::flushTlbAll();

        m_xhcBase = pciBarAddress;
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
} // namespace drivers
