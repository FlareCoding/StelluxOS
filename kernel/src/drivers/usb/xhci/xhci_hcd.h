#ifndef XHCI_HCD_H
#define XHCI_HCD_H

#include "xhci_device.h"

// Forward declaration
struct PciDeviceInfo;

class XhciHcd {
public:
    XhciHcd() = default;
    ~XhciHcd() = default;

    void init(PciDeviceInfo* deviceInfo);

    inline kstl::SharedPtr<XhciHcContext>& getCtx() { return m_ctx; }

    bool resetController();
    void startController();

    bool resetPort(uint8_t port);
    void resetAllPorts();

    void clearIrqFlags(uint8_t interrupter);

    XhciCommandCompletionTrb_t* sendCommand(XhciTrb_t* trb);

private:
    void _logUsbsts();
    void _identifyUsb3Ports();
    void _configureOperationalRegs();

    void _setupDevice(uint8_t port);

    uint8_t _enableSlot();
    bool _addressDevice(XhciDevice* device);

private:
    kstl::SharedPtr<XhciHcContext> m_ctx;
    kstl::SharedPtr<XhciDeviceContextManager> m_deviceContextManager;
};

#endif
