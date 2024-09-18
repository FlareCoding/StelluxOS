#ifndef XHCI_HCD_H
#define XHCI_HCD_H

#include "xhci_regs.h"
#include "xhci_mem.h"

struct XhciHcContext {
public:
    XhciHcContext(uint64_t xhcBase);
    ~XhciHcContext() = default;

    uint8_t getMaxDeviceSlots();
    uint8_t getMaxInterrupters();
    uint8_t getMaxPorts();

    uint8_t getIsochronousSchedulingThreshold();
    uint8_t getErstMax();
    uint8_t getMaxScratchpadBuffers();

    bool is64bitAddressable();
    bool hasBandwidthNegotiationCapability();
    bool has64ByteContextSize();
    bool hasPortPowerControl();
    bool hasPortIndicators();
    bool hasLightResetCapability();
    
    uint32_t getExtendedCapabilitiesOffset();

    uint64_t getXhcPageSize();

    void dumpCapabilityRegisters();

private:
    volatile XhciCapabilityRegisters* m_capRegs;
    volatile XhciOperationalRegisters* m_opRegs;
};

// Forward declaration
struct PciDeviceInfo;

class XhciHcd {
public:
    XhciHcd() = default;;
    ~XhciHcd() = default;

    void init(PciDeviceInfo* deviceInfo);

    inline kstl::SharedPtr<XhciHcContext>& getCtx() { return m_ctx; }

private:
    kstl::SharedPtr<XhciHcContext> m_ctx;
};

#endif
