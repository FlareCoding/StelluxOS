#ifndef XHCI_HID_H
#define XHCI_HID_H

#include <drivers/usb/usb_device_driver.h>
#include "xhci_regs.h"
#include "xhci_device.h"

class IHidDeviceDriver {
public:
    IHidDeviceDriver() = default;
    virtual ~IHidDeviceDriver() = default;

    virtual void handleEvent(uint8_t* data) = 0;
};

class XhciHidDriver : public IUsbDeviceDriver {
public:
    XhciHidDriver(XhciDoorbellManager* doorbellManager, XhciDevice* device);
    ~XhciHidDriver() = default;

    __force_inline__ XhciDevice* getDevice() { return m_device; }

    void start() override;
    void destroy() override;
    void handleEvent(void* evt) override;

private:
    XhciDevice*             m_device;
    XhciDoorbellManager*    m_doorbellManager;
    IHidDeviceDriver*       m_hidDeviceDriver;

    void _requestNextHidReport();
};

#endif