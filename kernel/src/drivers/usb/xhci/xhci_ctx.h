#ifndef XHCI_CTX_H
#define XHCI_CTX_H

#include "xhci_regs.h"
#include "xhci_mem.h"
#include "xhci_rings.h"
#include <kvector.h>

class XhciHcContext {
public:
    XhciHcContext(uint64_t xhcBase);
    ~XhciHcContext() = default;

    uint8_t getMaxDeviceSlots() const;
    uint8_t getMaxInterrupters() const;
    uint8_t getMaxPorts() const;

    uint8_t getIsochronousSchedulingThreshold() const;
    uint8_t getErstMax() const;
    uint8_t getMaxScratchpadBuffers() const;

    bool is64bitAddressable() const;
    bool hasBandwidthNegotiationCapability() const;
    bool has64ByteContextSize() const;
    bool hasPortPowerControl() const;
    bool hasPortIndicators() const;
    bool hasLightResetCapability() const;
    
    uint32_t getExtendedCapabilitiesOffset() const;
    uint64_t getXhcPageSize() const;

    bool isPortUsb3(uint8_t port);

    XhciPortRegisterManager getPortRegisterSet(uint8_t port);

    void dumpCapabilityRegisters();

public:
    uint64_t xhcBase;

    volatile XhciCapabilityRegisters*   capRegs;
    volatile XhciOperationalRegisters*  opRegs;
    volatile XhciRuntimeRegisters*      runtimeRegs;

    // Linked list of extended capabilities
    kstl::SharedPtr<XhciExtendedCapability> extendedCapabilitiesHead;

    // USB3.x-specific ports
    kstl::vector<uint8_t> usb3Ports;

    // Primary command ring
    kstl::SharedPtr<XhciCommandRing> commandRing;

    // Primary event ring
    kstl::SharedPtr<XhciEventRing> eventRing;

    // Doorbell register manager
    kstl::SharedPtr<XhciDoorbellManager> doorbellManager;
};

#endif