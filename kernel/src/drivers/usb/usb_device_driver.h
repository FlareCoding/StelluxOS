#ifndef USB_DEVICE_DRIVER_H
#define USB_DEVICE_DRIVER_H
#include <ktypes.h>

class IUsbDeviceDriver {
public:
    IUsbDeviceDriver() = default;
    virtual ~IUsbDeviceDriver() = default;

    virtual void start() = 0;
    virtual void destroy() = 0;
    virtual void handleEvent(void* evt) = 0;
};

#endif
