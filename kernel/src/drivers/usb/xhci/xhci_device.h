#ifndef XHCI_DEVICE_H
#define XHCI_DEVICE_H

#include "xhci_device_ctx.h"
#include "xhci_ctx.h"

class XhciDevice {
public:
    XhciDevice(XhciHcContext* xhc);
    XhciDevice(XhciHcContext* xhc, uint8_t port);
    ~XhciDevice() = default;

    void setupTransferRing();
    void setupAddressDeviceCtx(uint8_t portSpeed);

    uint64_t getInputContextPhysicalBase();

public:
    uint8_t port;
    uint8_t slotId;

    // Pointer to the entry in DCBAA
    XhciDma<XhciDeviceContext32>    deviceContext32;
    XhciDma<XhciDeviceContext64>    deviceContext64;

    // Input context buffer for configuring the device
    XhciDma<XhciInputContext32> inputContext32;
    XhciDma<XhciInputContext64> inputContext64;

    // Control endpoint's transfer ring
    kstl::SharedPtr<XhciTransferRing> controlEpTransferRing;

private:
    void _allocInputContext(XhciHcContext* xhc);
};

#endif
